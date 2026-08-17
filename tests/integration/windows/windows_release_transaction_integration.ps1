param(
    [Parameter(Mandatory = $true)][string]$Daemon,
    [Parameter(Mandatory = $true)][string]$Client,
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$ArtifactsDir
)

$ErrorActionPreference = "Stop"
$serviceName = "pm_tiny_release_test_$PID"
$pipeName = "\\.\pipe\$serviceName"
$runId = "run-$PID-$([Guid]::NewGuid().ToString('N'))"
$root = Join-Path $ArtifactsDir $runId
$sourceBin = Join-Path $root "source-bin"
$state = Join-Path $root "state"
$config = Join-Path $state "pm_tiny.yaml"
$programConfig = Join-Path $state "prog.yaml"
$deploy = Join-Path $SourceDir "scripts/release/deploy-windows.ps1"
$oldFailureBefore = $env:PM_TINY_RELEASE_TEST_FAIL_BEFORE_SWITCH
$oldFailureAfter = $env:PM_TINY_RELEASE_TEST_FAIL_AFTER_SWITCH
$oldInterrupt = $env:PM_TINY_RELEASE_TEST_INTERRUPT_AFTER_SWITCH

function Invoke-Deploy([hashtable]$Arguments) {
    & $deploy -Root $root -ConfigPath $config -HomePath $state -PipeName $pipeName `
        -ServiceName $serviceName @Arguments
}

function Assert-Current([string]$Expected) {
    $actual = (Get-Content -LiteralPath (Join-Path $root "current.release") -Raw).Trim()
    if ($actual -ne $Expected) { throw "expected current $Expected, got $actual" }
}

try {
    New-Item -ItemType Directory -Force -Path $sourceBin, $state | Out-Null
    Copy-Item -LiteralPath $Daemon -Destination (Join-Path $sourceBin "pm_tiny.exe")
    Copy-Item -LiteralPath $Client -Destination (Join-Path $sourceBin "pm.exe")
    [IO.File]::WriteAllText($programConfig, "[]`r`n", [Text.UTF8Encoding]::new($false))
    $yaml = "pm_tiny_prog_cfg_file: prog.yaml`r`npm_tiny_log_file: daemon.log`r`n" +
        "pm_tiny_app_log_dir: logs`r`npm_tiny_app_environ_dir: environ`r`n"
    [IO.File]::WriteAllText($config, $yaml, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $state "sentinel.txt"), "preserve", [Text.UTF8Encoding]::new($false))

    Invoke-Deploy @{ ReleaseId = "release-a"; SourceDir = $sourceBin }
    Assert-Current "release-a"

    $env:PM_TINY_RELEASE_TEST_FAIL_BEFORE_SWITCH = "1"
    $failed = $false
    try { Invoke-Deploy @{ ReleaseId = "release-before-fail"; SourceDir = $sourceBin } }
    catch { $failed = $true }
    $env:PM_TINY_RELEASE_TEST_FAIL_BEFORE_SWITCH = $null
    if (-not $failed) { throw "pre-switch failure injection unexpectedly succeeded" }
    Assert-Current "release-a"

    Invoke-Deploy @{ ReleaseId = "release-b"; SourceDir = $sourceBin }
    Assert-Current "release-b"

    $env:PM_TINY_RELEASE_TEST_FAIL_AFTER_SWITCH = "1"
    $failed = $false
    try { Invoke-Deploy @{ ReleaseId = "release-after-fail"; SourceDir = $sourceBin } }
    catch { $failed = $true }
    $env:PM_TINY_RELEASE_TEST_FAIL_AFTER_SWITCH = $null
    if (-not $failed) { throw "post-switch failure injection unexpectedly succeeded" }
    Assert-Current "release-b"

    $env:PM_TINY_RELEASE_TEST_INTERRUPT_AFTER_SWITCH = "1"
    $interrupted = $false
    try {
        Invoke-Deploy @{ ReleaseId = "release-interrupted"; SourceDir = $sourceBin }
    } catch {
        $interrupted = $true
    }
    $env:PM_TINY_RELEASE_TEST_INTERRUPT_AFTER_SWITCH = $null
    if (-not $interrupted -or -not (Test-Path -LiteralPath (Join-Path $root "journal.json"))) {
        throw "interruption injection did not leave a recoverable journal"
    }
    Assert-Current "release-interrupted"

    Invoke-Deploy @{ Action = "Rollback"; TargetReleaseId = "release-a" }
    Assert-Current "release-a"
    if (Test-Path -LiteralPath (Join-Path $root "journal.json")) { throw "journal was not cleared" }
    if ((Get-Content -LiteralPath (Join-Path $state "sentinel.txt") -Raw) -ne "preserve") {
        throw "state outside releases was modified"
    }
    $escaped = $serviceName.Replace("'", "''")
    $servicePath = (Get-CimInstance Win32_Service -Filter "Name='$escaped'").PathName
    if (-not $servicePath.Contains("release-a\pm_tiny.exe")) {
        throw "SCM BinaryPath was not rolled back: $servicePath"
    }
    Write-Host "windows release transaction integration: PASS"
} finally {
    $env:PM_TINY_RELEASE_TEST_FAIL_BEFORE_SWITCH = $oldFailureBefore
    $env:PM_TINY_RELEASE_TEST_FAIL_AFTER_SWITCH = $oldFailureAfter
    $env:PM_TINY_RELEASE_TEST_INTERRUPT_AFTER_SWITCH = $oldInterrupt
    if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) {
        & (Join-Path $SourceDir "scripts/windows/uninstall-service.ps1") -Name $serviceName
    }
}
