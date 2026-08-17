#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [ValidateSet("Deploy", "Rollback")][string]$Action = "Deploy",
    [string]$ReleaseId,
    [string]$SourceDir,
    [string]$TargetReleaseId,
    [Parameter(Mandatory = $true)][string]$ConfigPath,
    [Parameter(Mandatory = $true)][string]$HomePath,
    [Parameter(Mandatory = $true)][string]$PipeName,
    [Parameter(Mandatory = $true)][string]$ServiceName,
    [switch]$KeepRunning
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ManifestTool = Join-Path $ScriptDir "release_manifest.py"
$Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
$driveRoot = [IO.Path]::GetPathRoot($Root).TrimEnd('\')
if ($Root.Equals($driveRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "unsafe release root: $Root"
}

function Assert-ReleaseId([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value) -or
        $Value -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or $Value.Contains('..')) {
        throw "unsafe release id: $Value"
    }
}

if ($Action -eq "Deploy") {
    Assert-ReleaseId $ReleaseId
    if (-not (Test-Path -LiteralPath (Join-Path $SourceDir "pm_tiny.exe")) -or
        -not (Test-Path -LiteralPath (Join-Path $SourceDir "pm.exe"))) {
        throw "source directory must contain pm_tiny.exe and pm.exe"
    }
} else {
    Assert-ReleaseId $TargetReleaseId
}

$Releases = Join-Path $Root "releases"
$State = [IO.Path]::GetFullPath($HomePath)
$Current = Join-Path $Root "current.release"
$Journal = Join-Path $Root "journal.json"
New-Item -ItemType Directory -Force -Path $Releases, $State | Out-Null
$ConfigPath = (Resolve-Path -LiteralPath $ConfigPath).Path

$Python = Get-Command python.exe -ErrorAction SilentlyContinue
$PythonPrefix = @()
if (-not $Python) {
    $Python = Get-Command py.exe -ErrorAction Stop
    $PythonPrefix = @("-3")
}

function Invoke-Manifest([string[]]$Arguments) {
    & $Python.Source @PythonPrefix $ManifestTool @Arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "release manifest command failed" }
}

function Get-CurrentRelease {
    if (Test-Path -LiteralPath $Current) { return (Get-Content -LiteralPath $Current -Raw).Trim() }
    return ""
}

function Set-CurrentRelease([string]$Value) {
    $temporary = "$Current.tmp.$PID"
    [IO.File]::WriteAllText($temporary, "$Value`r`n", [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $Current -Force
}

function Clear-CurrentRelease {
    if (Test-Path -LiteralPath $Current) { Remove-Item -LiteralPath $Current -Force }
}

function Write-Journal([hashtable]$Data) {
    $temporary = "$Journal.tmp.$PID"
    $Data.schema_version = 1
    [IO.File]::WriteAllText($temporary, (($Data | ConvertTo-Json -Compress) + "`r`n"),
        [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $Journal -Force
}

function Get-ServicePath {
    $escaped = $ServiceName.Replace("'", "''")
    $instance = Get-CimInstance Win32_Service -Filter "Name='$escaped'" -ErrorAction SilentlyContinue
    if ($instance) { return $instance.PathName }
    return ""
}

function Stop-TestService {
    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($service -and $service.Status -ne 'Stopped') {
        Stop-Service -Name $ServiceName -Force
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    }
}

function Set-ServiceBinary([string]$BinaryPath) {
    & sc.exe config $ServiceName binPath= $BinaryPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "failed to update service BinaryPath" }
}

function Remove-TestService {
    Stop-TestService
    & sc.exe delete $ServiceName | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "failed to delete service $ServiceName" }
}

function Invoke-ClientInfo([string]$Client) {
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "SilentlyContinue"
        $output = & $Client info --json 2>$null | Out-String
        return [PSCustomObject]@{ ExitCode = $LASTEXITCODE; Output = $output }
    } finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Restore-Transaction($Transaction) {
    Stop-TestService
    if ($Transaction.service_existed) {
        Set-ServiceBinary ([string]$Transaction.old_binary_path)
        if ($Transaction.old_was_running) {
            Start-Service -Name $ServiceName
            (Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
        }
    } elseif (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        Remove-TestService
    }
    if ([string]::IsNullOrEmpty([string]$Transaction.old_release)) {
        Clear-CurrentRelease
    } else {
        Set-CurrentRelease ([string]$Transaction.old_release)
    }
}

if (Test-Path -LiteralPath $Journal) {
    $pending = Get-Content -LiteralPath $Journal -Raw | ConvertFrom-Json
    if ($pending.schema_version -ne 1) { throw "unsupported release journal" }
    if ($pending.phase -eq "switched") { Restore-Transaction $pending }
    elseif ($pending.phase -ne "prepared") { throw "unsupported release journal phase" }
    Remove-Item -LiteralPath $Journal -Force
    Write-Host "recovered interrupted transaction: $($pending.new_release) -> $($pending.old_release)"
}

function Test-ReleaseHealth([string]$ReleaseDir, [string]$Tag) {
    $healthRoot = Join-Path $State ("health-{0}-{1}-{2}" -f $Tag, $PID, [Guid]::NewGuid().ToString('N'))
    $healthPipe = "\\.\pipe\pm_tiny-release-$ServiceName-$PID-$Tag"
    $daemon = Join-Path $ReleaseDir "pm_tiny.exe"
    $client = Join-Path $ReleaseDir "pm.exe"
    New-Item -ItemType Directory -Force -Path $healthRoot | Out-Null
    [IO.File]::WriteAllText((Join-Path $healthRoot "prog.yaml"), "[]`r`n", [Text.UTF8Encoding]::new($false))
    $healthConfig = Join-Path $healthRoot "pm_tiny.yaml"
    $yaml = "pm_tiny_prog_cfg_file: prog.yaml`r`npm_tiny_log_file: daemon.log`r`n" +
        "pm_tiny_app_log_dir: logs`r`npm_tiny_app_environ_dir: environ`r`n"
    [IO.File]::WriteAllText($healthConfig, $yaml, [Text.UTF8Encoding]::new($false))
    $oldHome = $env:PM_TINY_HOME
    $oldPipe = $env:PM_TINY_PIPE_NAME
    $process = $null
    try {
        $env:PM_TINY_HOME = $healthRoot
        $env:PM_TINY_PIPE_NAME = $healthPipe
        $arguments = '--home "' + $healthRoot + '" --config "' + $healthConfig + '" --pipe-name "' + $healthPipe + '"'
        $process = Start-Process -FilePath $daemon -ArgumentList $arguments -PassThru -WindowStyle Hidden
        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            $result = Invoke-ClientInfo $client
            if ($result.ExitCode -eq 0) {
                $info = $result.Output | ConvertFrom-Json
                if ($info.identity.protocol_version -eq 3 -and $info.runtime.state -eq "running") { return $true }
            }
            if ($process.HasExited) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        return $false
    } finally {
        if ($process -and -not $process.HasExited) {
            try { & $client quit *> $null } catch { }
            if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
        }
        $env:PM_TINY_HOME = $oldHome
        $env:PM_TINY_PIPE_NAME = $oldPipe
        Remove-Item -LiteralPath $healthRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$OldRelease = Get-CurrentRelease
$ExistingService = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
$OldWasRunning = [bool]($ExistingService -and $ExistingService.Status -ne 'Stopped')
$OldBinaryPath = Get-ServicePath

if ($Action -eq "Deploy") {
    $Target = Join-Path $Releases $ReleaseId
    if (Test-Path -LiteralPath $Target) { throw "release already exists: $ReleaseId" }
    $Staging = Join-Path $Root (".staging-$ReleaseId-$PID")
    try {
        New-Item -ItemType Directory -Path $Staging | Out-Null
        Copy-Item -LiteralPath (Join-Path $SourceDir "pm_tiny.exe"), (Join-Path $SourceDir "pm.exe") -Destination $Staging
        $versionOutput = & (Join-Path $Staging "pm_tiny.exe") --version | Out-String
        if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch 'pm_tiny\s+([^\s]+)') { throw "daemon version probe failed" }
        $Version = $Matches[1]
        Invoke-Manifest @("create", "--release-dir", $Staging, "--release-id", $ReleaseId,
            "--version", $Version, "--platform", "windows", "--arch", $env:PROCESSOR_ARCHITECTURE)
        Invoke-Manifest @("verify", "--release-dir", $Staging, "--release-id", $ReleaseId, "--platform", "windows")
        if (-not (Test-ReleaseHealth $Staging "pre-switch")) { throw "pre-switch health check failed" }
        if ($env:PM_TINY_RELEASE_TEST_FAIL_BEFORE_SWITCH -eq "1") { throw "injected pre-switch failure" }
        Move-Item -LiteralPath $Staging -Destination $Target
        $Staging = $null
    } finally {
        if ($Staging -and (Test-Path -LiteralPath $Staging)) {
            Remove-Item -LiteralPath $Staging -Recurse -Force
        }
    }
    $NewRelease = $ReleaseId
} else {
    $NewRelease = $TargetReleaseId
    $Target = Join-Path $Releases $NewRelease
    if (-not (Test-Path -LiteralPath $Target)) { throw "rollback target does not exist: $NewRelease" }
    Invoke-Manifest @("verify", "--release-dir", $Target, "--release-id", $NewRelease, "--platform", "windows")
    if (-not (Test-ReleaseHealth $Target "pre-rollback")) { throw "rollback target health check failed" }
}

if ($OldRelease -eq $NewRelease) { throw "release is already current: $NewRelease" }
$transaction = @{
    phase = "prepared"; old_release = $OldRelease; new_release = $NewRelease
    service_existed = [bool]$ExistingService; old_binary_path = $OldBinaryPath
    old_was_running = $OldWasRunning
}
Write-Journal $transaction
Stop-TestService
$newCommand = '"' + (Join-Path $Target "pm_tiny.exe") + '" --service --service-name "' + $ServiceName +
    '" --config "' + $ConfigPath + '"'
if ($ExistingService) {
    Set-ServiceBinary $newCommand
} else {
    & (Join-Path (Split-Path -Parent $ScriptDir) "windows\install-service.ps1") `
        -BinaryPath (Join-Path $Target "pm_tiny.exe") -ConfigPath $ConfigPath -Name $ServiceName `
        -DisplayName $ServiceName -StartupType Manual -HomePath $State -PipeName $PipeName | Out-Null
}
Set-CurrentRelease $NewRelease
$transaction.phase = "switched"
Write-Journal $transaction

if ($env:PM_TINY_RELEASE_TEST_INTERRUPT_AFTER_SWITCH -eq "1") {
    throw "injected interruption after switch"
}

$postHealthy = $false
if ($env:PM_TINY_RELEASE_TEST_FAIL_AFTER_SWITCH -ne "1") {
    $oldPipe = $env:PM_TINY_PIPE_NAME
    try {
        Start-Service -Name $ServiceName
        (Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
        $env:PM_TINY_PIPE_NAME = $PipeName
        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            $result = Invoke-ClientInfo (Join-Path $Target "pm.exe")
            if ($result.ExitCode -eq 0) {
                $info = $result.Output | ConvertFrom-Json
                if ($info.identity.protocol_version -eq 3 -and $info.runtime.mode -eq "service") {
                    $postHealthy = $true
                    break
                }
            }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
    } catch {
        $postHealthy = $false
    } finally {
        $env:PM_TINY_PIPE_NAME = $oldPipe
    }
}

if (-not $postHealthy) {
    Restore-Transaction ([pscustomobject]$transaction)
    Remove-Item -LiteralPath $Journal -Force
    throw "post-switch health check failed; restored previous release"
}
if (-not $KeepRunning -and -not $OldWasRunning) { Stop-TestService }
Remove-Item -LiteralPath $Journal -Force
Write-Host "current release: $NewRelease"
