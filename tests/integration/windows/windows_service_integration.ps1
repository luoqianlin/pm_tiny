param(
    [Parameter(Mandatory = $true)][string]$Daemon,
    [Parameter(Mandatory = $true)][string]$Client,
    [Parameter(Mandatory = $true)][string]$TreeFixture,
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$ArtifactsDir
)

$ErrorActionPreference = "Stop"
$serviceName = "pm_tiny_test_$PID"
$pipeName = "\\.\pipe\$serviceName"
$work = Join-Path $ArtifactsDir "service"
$config = Join-Path $work "pm_tiny.yaml"
$programConfig = Join-Path $work "prog.yaml"
$pidFile = Join-Path $work "pids.txt"
$markerFile = Join-Path $work "marker.txt"
$previousPipe = $env:PM_TINY_PIPE_NAME
$currentSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$pipeSddl = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;$currentSid)"

function Invoke-Pm([string[]]$Arguments) {
    $env:PM_TINY_PIPE_NAME = $pipeName
    $output = & $Client @Arguments 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "pm failed: $output" }
    return $output
}

function Wait-For([scriptblock]$Condition, [int]$TimeoutSeconds, [string]$Description) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Description"
}

try {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $command = '"' + $TreeFixture + '" --mode resistant --pid-file "' + $pidFile +
        '" --marker-file "' + $markerFile + '"'
    $yaml = @"
- name: service_tree
  executable: '$($TreeFixture.Replace("'", "''"))'
  args: ["--mode", "resistant", "--pid-file", '$($pidFile.Replace("'", "''"))', "--marker-file", '$($markerFile.Replace("'", "''"))']
  cwd: '$($work.Replace("'", "''"))'
  daemon: false
  kill_timeout: 2
"@
    [System.IO.File]::WriteAllText($programConfig, $yaml, [System.Text.UTF8Encoding]::new($false))
    $daemonYaml = @"
pm_tiny_log_file: pm_tiny.log
pm_tiny_app_log_dir: logs
pm_tiny_prog_cfg_file: prog.yaml
pm_tiny_log_level: info
pm_tiny_log_max_size_kb: 4096
pm_tiny_log_archive_count: 3
"@
    [System.IO.File]::WriteAllText($config, $daemonYaml, [System.Text.UTF8Encoding]::new($false))
    & (Join-Path $SourceDir "scripts/windows/install-service.ps1") `
        -BinaryPath $Daemon -ConfigPath $config -Name $serviceName `
        -DisplayName $serviceName -StartupType Manual -PipeName $pipeName `
        -PipeSddl $pipeSddl -HomePath $work -Start | Out-Null
    $serviceEnvironment = (Get-ItemProperty `
        "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName" -Name Environment).Environment
    if ($serviceEnvironment -notcontains "PM_TINY_HOME=$work") {
        throw "service environment did not contain PM_TINY_HOME=$work"
    }
    if ($serviceEnvironment -notcontains "PM_TINY_PIPE_NAME=$pipeName") {
        throw "service environment did not contain PM_TINY_PIPE_NAME=$pipeName"
    }
    $service = Get-Service -Name $serviceName
    if ($service.Status -ne 'Running') { throw "service did not reach Running" }
    Wait-For {
        try { (Invoke-Pm @("version")).Contains("pm_tiny: 4.1.0") } catch { $false }
    } 10 "service control pipe"
    $daemonInfo = Invoke-Pm @("info", "--json") | ConvertFrom-Json
    if ($daemonInfo.runtime.mode -ne "service" -or
        $daemonInfo.ipc.named_pipe.value -ne $pipeName -or
        $daemonInfo.process_tree.effective_mode -ne "job_object") {
        throw "service daemon info mismatch"
    }
    Wait-For { (Test-Path $pidFile) -and ((Get-Content $pidFile).Count -eq 2) } 10 "service process tree"
    $processIds = @(Get-Content $pidFile | ForEach-Object { [int]$_ })
    Stop-Service -Name $serviceName
    $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    foreach ($processId in $processIds) {
        if (Get-Process -Id $processId -ErrorAction SilentlyContinue) {
            throw "service stop left process $processId running"
        }
    }
    Write-Host "windows service integration: PASS"
} finally {
    & (Join-Path $SourceDir "scripts/windows/uninstall-service.ps1") -Name $serviceName
    $env:PM_TINY_PIPE_NAME = $previousPipe
}
