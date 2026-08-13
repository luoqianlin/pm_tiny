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
  command: '$($command.Replace("'", "''"))'
  cwd: '$($work.Replace("'", "''"))'
  daemon: false
  kill_timeout: 2
"@
    [System.IO.File]::WriteAllText($config, $yaml, [System.Text.UTF8Encoding]::new($false))
    & (Join-Path $SourceDir "scripts/windows/install-service.ps1") `
        -BinaryPath $Daemon -ConfigPath $config -Name $serviceName `
        -DisplayName $serviceName -StartupType Manual -PipeName $pipeName `
        -PipeSddl $pipeSddl -Start | Out-Null
    $service = Get-Service -Name $serviceName
    if ($service.Status -ne 'Running') { throw "service did not reach Running" }
    Wait-For {
        try { (Invoke-Pm @("version")).Contains("Windows v2") } catch { $false }
    } 10 "service control pipe"
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
