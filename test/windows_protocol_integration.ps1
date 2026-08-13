param(
    [Parameter(Mandatory = $true)][string]$Daemon,
    [Parameter(Mandatory = $true)][string]$Cli,
    [Parameter(Mandatory = $true)][string]$SdkProbe,
    [Parameter(Mandatory = $true)][string]$ProtocolProbe,
    [Parameter(Mandatory = $true)][string]$TreeFixture,
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$ArtifactsDir
)

$ErrorActionPreference = "Stop"
$script:DaemonProcess = $null
$script:CurrentScenario = ""
$script:PreviousPipeName = $env:PM_TINY_PIPE_NAME

function Assert-Contains([string]$Actual, [string]$Expected, [string]$Context) {
    if (-not $Actual.Contains($Expected)) {
        throw "$Context did not contain '$Expected'. Actual: $Actual"
    }
}

function Invoke-Pm([string[]]$Arguments) {
    Write-Host "[windows-integration] pm $($Arguments -join ' ')"
    $output = & $Cli @Arguments 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "pm $($Arguments -join ' ') failed with exit code $LASTEXITCODE. Output: $output"
    }
    return $output
}

function Invoke-PmFailure([string[]]$Arguments) {
    Write-Host "[windows-integration] pm $($Arguments -join ' ') (expect failure)"
    $stdoutPath = Join-Path $ArtifactsDir "connection-failure-cli.stdout.log"
    $stderrPath = Join-Path $ArtifactsDir "connection-failure-cli.stderr.log"
    $process = Start-Process -FilePath $Cli -ArgumentList $Arguments `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru -Wait
    $stdout = Get-Content $stdoutPath -Raw -ErrorAction SilentlyContinue
    $stderr = Get-Content $stderrPath -Raw -ErrorAction SilentlyContinue
    if ($process.ExitCode -ne 1) {
        throw "pm $($Arguments -join ' ') expected exit code 1, got $($process.ExitCode). stderr: $stderr"
    }
    if (-not [string]::IsNullOrEmpty($stdout)) {
        throw "pm $($Arguments -join ' ') expected empty stdout. Actual: $stdout"
    }
    return $stderr
}

function Wait-For([scriptblock]$Condition, [int]$TimeoutSeconds, [string]$Description) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Description"
}

function Wait-For-State([string]$Name, [string]$State, [int]$TimeoutSeconds, [string]$Description) {
    Wait-For {
        $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        @($status.processes | Where-Object { $_.name -eq $Name -and $_.state -eq $State }).Count -eq 1
    } $TimeoutSeconds $Description
}

function Start-TestDaemon([string]$Scenario, [string]$ConfigPath) {
    Write-Host "[windows-integration] starting $Scenario"
    $script:CurrentScenario = $Scenario
    $env:PM_TINY_PIPE_NAME = "\\.\pipe\pm_tiny_test_$PID`_$Scenario"
    $stdout = Join-Path $ArtifactsDir "$Scenario-daemon.stdout.log"
    $stderr = Join-Path $ArtifactsDir "$Scenario-daemon.stderr.log"
    $quotedConfigPath = '"' + $ConfigPath + '"'
    $script:DaemonProcess = Start-Process -FilePath $Daemon -ArgumentList @("--config", $quotedConfigPath) `
        -WorkingDirectory $ArtifactsDir -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    Wait-For {
        try { (Invoke-Pm @("version")).Contains("Windows v2") } catch { $false }
    } 10 "$Scenario daemon control pipe"
}

function Stop-TestDaemon([bool]$RequireCleanExit = $true) {
    if ($null -eq $script:DaemonProcess) { return }
    $script:DaemonProcess.Refresh()
    if (-not $script:DaemonProcess.HasExited) {
        try { [void](Invoke-Pm @("quit")) } catch { }
        $script:DaemonProcess.Refresh()
        if (-not $script:DaemonProcess.WaitForExit(10000)) {
            $script:DaemonProcess.Kill()
            $script:DaemonProcess.WaitForExit()
            if ($RequireCleanExit) { throw "$script:CurrentScenario daemon did not exit after quit" }
        } else {
            $script:DaemonProcess.WaitForExit()
        }
    }
    $script:DaemonProcess.Refresh()
    $exitCode = $script:DaemonProcess.ExitCode
    if ($RequireCleanExit -and $null -ne $exitCode -and $exitCode -ne 0) {
        throw "$script:CurrentScenario daemon exited with $exitCode"
    }
    $script:DaemonProcess = $null
}

function Write-Utf8NoBom([string]$Path, [string]$Content) {
    [System.IO.File]::WriteAllText($Path, $Content, [System.Text.UTF8Encoding]::new($false))
}

function Assert-ConnectionFailure([string]$PipeName, [string]$Context) {
    $previous = $env:PM_TINY_PIPE_NAME
    try {
        $env:PM_TINY_PIPE_NAME = $PipeName
        $output = Invoke-PmFailure @("list")
        Assert-Contains $output "pm: cannot connect to pm_tiny" "$Context summary"
        Assert-Contains $output "endpoint: $PipeName" "$Context endpoint"
        Assert-Contains $output "transport: Windows named pipe" "$Context transport"
        Assert-Contains $output "winerror=" "$Context error code"
        Assert-Contains $output "hint:" "$Context hint"
    } finally {
        $env:PM_TINY_PIPE_NAME = $previous
    }
}

function Run-ConnectionFailureScenario {
    Write-Host "[windows-integration] connection failure scenario"
    Assert-ConnectionFailure "\\.\pipe\pm_tiny_missing_$PID" "daemon not running"

    $work = Join-Path $ArtifactsDir "connection-failure"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-Utf8NoBom $config "[]`n"
    Start-TestDaemon "connection-failure" $config
    Assert-ConnectionFailure "\\.\pipe\pm_tiny_wrong_$PID" "wrong pipe name"
    Stop-TestDaemon
}

function Yaml-Quote([string]$Value) {
    return "'" + $Value.Replace("'", "''") + "'"
}

function Expand-Fixture([string]$Name, [string]$Destination, [hashtable]$Values) {
    $content = Get-Content (Join-Path $SourceDir "test/$Name") -Raw
    foreach ($entry in $Values.GetEnumerator()) {
        $content = $content.Replace("__$($entry.Key)__", $entry.Value)
    }
    Write-Utf8NoBom $Destination $content
}

function Run-MainProtocolScenario {
    $work = Join-Path $ArtifactsDir "main"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $logDir = Join-Path $work "logs"
    $config = Join-Path $work "pm_tiny.yaml"
    $command = 'powershell.exe -NoProfile -Command Start-Sleep -Seconds 60'
    $yaml = @"
- name: app
  command: $(Yaml-Quote $command)
  cwd: $(Yaml-Quote $work)
  daemon: false
  log_dir: $(Yaml-Quote $logDir)
  log_file_name: app.log
"@
    Write-Utf8NoBom $config $yaml
    Start-TestDaemon "main" $config
    Write-Host "[windows-integration] main commands"

    Assert-Contains (Invoke-Pm @("version")) "Windows v2" "version"
    Assert-Contains (Invoke-Pm @("list")) "app" "list"
    Assert-Contains (Invoke-Pm @("inspect", "app")) "name=app" "inspect"
    Assert-Contains (Invoke-Pm @("restart", "app")) "OK" "restart"
    Assert-Contains (Invoke-Pm @("stop", "app")) "OK" "stop"
    Wait-For-State "app" "stopped" 10 "app stopped state"

    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $largeLog = "LOG_MARKER`n" + ("x" * 2200000)
    Write-Utf8NoBom (Join-Path $logDir "app.log") $largeLog
    $logOutput = Invoke-Pm @("log", "app")
    Assert-Contains $logOutput "LOG_MARKER" "streamed log"
    if ($logOutput.Length -lt 2200000) { throw "streamed log was truncated" }

    Assert-Contains (Invoke-Pm @("start", "app")) "OK started" "start"
    Assert-Contains (Invoke-Pm @("save")) "OK saved" "save"
    if (Test-Path "$config.tmp") { throw "transactional save left a temporary file" }
    Assert-Contains (Invoke-Pm @("reload")) "OK reloaded" "reload"
    Assert-Contains (Invoke-Pm @("delete", "app")) "OK deleted" "delete"
    Assert-Contains (Invoke-Pm @("inspect", "app")) "ERR process not found" "missing inspect"
    Assert-Contains (Invoke-Pm @("stop", "missing")) "ERR process not found" "missing stop"
    Assert-Contains (Invoke-Pm @("start", "missing")) "ERR missing command" "missing start command"
    Assert-Contains (Invoke-Pm @("save")) "OK saved" "empty save"
    Assert-Contains (Invoke-Pm @("reload")) "OK reloaded" "empty reload"

    foreach ($mode in @("fragmented", "coalesced", "unknown-type", "invalid-magic", "invalid-version", "invalid-flags", "oversize", "slow")) {
        & $ProtocolProbe $mode
        if ($LASTEXITCODE -ne 0) { throw "protocol probe failed: $mode" }
        Assert-Contains (Invoke-Pm @("version")) "Windows v2" "daemon recovery after $mode"
    }
    Stop-TestDaemon
}

function Run-DependencyScenario {
    Write-Host "[windows-integration] dependency scenario"
    $work = Join-Path $ArtifactsDir "dependency"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Expand-Fixture "windows_dependency.yaml" $config @{ WORK_DIR = $work }
    Start-TestDaemon "dependency" $config
    Wait-For-State "child" "online" 10 "dependency child online state"
    $list = Invoke-Pm @("list")
    if ($list.IndexOf("base") -lt 0 -or $list.IndexOf("child") -lt 0 -or $list.IndexOf("base") -gt $list.IndexOf("child")) {
        throw "dependency order was not topological. Actual: $list"
    }
    $graph = Invoke-Pm @("graph", "--no-color")
    Assert-Contains $graph "Dependency graph: 2 nodes, 1 edge" "dependency graph summary"
    Assert-Contains $graph "child [online] <- base" "dependency graph edge"
    $graphJson = Invoke-Pm @("graph", "base", "--json") | ConvertFrom-Json
    if ($graphJson.focus -ne "base" -or @($graphJson.nodes).Count -ne 2 -or @($graphJson.edges).Count -ne 1) {
        throw "focused dependency graph JSON was invalid"
    }
    $dot = Invoke-Pm @("dag", "--dot")
    Assert-Contains $dot '"base" -> "child";' "dependency graph DOT edge"
    $missing = Invoke-PmFailure @("graph", "missing")
    Assert-Contains $missing "process not found: missing" "missing graph focus"
    Stop-TestDaemon
}

function Run-SdkScenario {
    Write-Host "[windows-integration] sdk scenario"
    $work = Join-Path $ArtifactsDir "sdk"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Expand-Fixture "windows_sdk_managed.yaml" $config @{
        SDK_PROBE = ('"' + $SdkProbe + '"')
        WORK_DIR = $work
        LOG_DIR = (Join-Path $work "logs")
    }
    Start-TestDaemon "sdk" $config
    Wait-For {
        $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        @($status.processes | Where-Object { $_.name -eq "sdk_probe" -and $_.state -eq "online" }).Count -eq 1
    } 5 "SDK ready signal"
    Wait-For {
        $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        @($status.processes | Where-Object { $_.name -eq "sdk_probe" -and $_.state -eq "stopped" }).Count -eq 1
    } 8 "SDK heartbeat timeout"
    $daemonLog = Get-Content (Join-Path $ArtifactsDir "sdk-daemon.stderr.log") -Raw
    Assert-Contains $daemonLog "heartbeat timeout" "SDK heartbeat policy"
    Stop-TestDaemon
}

function Run-StartTimeoutScenario {
    Write-Host "[windows-integration] start-timeout scenario"
    $work = Join-Path $ArtifactsDir "start-timeout"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Expand-Fixture "windows_start_timeout.yaml" $config @{ WORK_DIR = $work }
    Start-TestDaemon "start-timeout" $config
    Wait-For {
        $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        @($status.processes | Where-Object { $_.name -eq "never_ready" -and $_.state -eq "failed" }).Count -eq 1
    } 8 "start timeout termination"
    $daemonLog = Get-Content (Join-Path $ArtifactsDir "start-timeout-daemon.stderr.log") -Raw
    Assert-Contains $daemonLog "start timeout" "start timeout policy"
    Stop-TestDaemon
}

function Run-UnsupportedScenario {
    Write-Host "[windows-integration] unsupported scenario"
    $work = Join-Path $ArtifactsDir "unsupported"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Expand-Fixture "windows_unsupported.yaml" $config @{ WORK_DIR = $work }
    Start-TestDaemon "unsupported" $config
    $daemonLog = Get-Content (Join-Path $ArtifactsDir "unsupported-daemon.stderr.log") -Raw
    Assert-Contains $daemonLog "pty: true" "unsupported Windows configuration"
    Stop-TestDaemon
}

function Assert-Processes-Gone([int[]]$ProcessIds, [string]$Context) {
    foreach ($processId in $ProcessIds) {
        if (Get-Process -Id $processId -ErrorAction SilentlyContinue) {
            throw "$Context left process $processId running"
        }
    }
}

function Run-ProcessTreeCase([string]$Mode, [int]$KillTimeout) {
    Write-Host "[windows-integration] process-tree $Mode scenario"
    $work = Join-Path $ArtifactsDir "tree-$Mode"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    $pidFile = Join-Path $work "pids.txt"
    $markerFile = Join-Path $work "ctrl-break.txt"
    $command = '"' + $TreeFixture + '" --mode ' + $Mode +
        ' --pid-file "' + $pidFile + '" --marker-file "' + $markerFile + '"'
    $yaml = @"
- name: tree_fixture
  command: $(Yaml-Quote $command)
  cwd: $(Yaml-Quote $work)
  daemon: false
  kill_timeout: $KillTimeout
"@
    Write-Utf8NoBom $config $yaml
    Start-TestDaemon "tree-$Mode" $config
    Wait-For { (Test-Path $pidFile) -and ((Get-Content $pidFile).Count -eq 2) } 5 "$Mode pid file"
    $processIds = @(Get-Content $pidFile | ForEach-Object { [int]$_ })
    $started = [DateTime]::UtcNow
    Assert-Contains (Invoke-Pm @("stop", "tree_fixture")) "OK stop requested" "$Mode stop"
    $versionStarted = [DateTime]::UtcNow
    Assert-Contains (Invoke-Pm @("version")) "Windows v2" "$Mode version during termination"
    if (([DateTime]::UtcNow - $versionStarted).TotalSeconds -gt 1.0) {
        throw "$Mode version was blocked by termination"
    }
    Wait-For-State "tree_fixture" "stopped" 8 "$Mode tree stopped state"
    $elapsed = ([DateTime]::UtcNow - $started).TotalSeconds
    Assert-Processes-Gone $processIds "$Mode termination"
    if ($Mode -eq "graceful") {
        Assert-Contains (Get-Content "$markerFile.parent" -Raw) "parent=" "graceful parent CTRL_BREAK"
        Assert-Contains (Get-Content "$markerFile.child" -Raw) "child=" "graceful child CTRL_BREAK"
        if ($elapsed -ge $KillTimeout) { throw "graceful termination exceeded the grace period" }
    } else {
        if ($elapsed -lt ($KillTimeout - 0.25)) { throw "$Mode was force-killed before the grace period" }
    }
    Stop-TestDaemon
}

function Run-RapidRestartScenario {
    Write-Host "[windows-integration] rapid restart scenario"
    $work = Join-Path $ArtifactsDir "rapid-restart"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    $pidFile = Join-Path $work "pids.txt"
    $markerFile = Join-Path $work "ctrl-break.txt"
    $command = '"' + $TreeFixture + '" --mode resistant --pid-file "' + $pidFile +
        '" --marker-file "' + $markerFile + '"'
    $yaml = @"
- name: restart_fixture
  command: $(Yaml-Quote $command)
  cwd: $(Yaml-Quote $work)
  daemon: false
  kill_timeout: 1
"@
    Write-Utf8NoBom $config $yaml
    Start-TestDaemon "rapid-restart" $config
    Wait-For { (Test-Path $pidFile) -and ((Get-Content $pidFile).Count -eq 2) } 5 "initial restart pids"
    $oldPids = @(Get-Content $pidFile | ForEach-Object { [int]$_ })
    Assert-Contains (Invoke-Pm @("restart", "restart_fixture")) "OK restart requested" "first restart"
    Assert-Contains (Invoke-Pm @("restart", "restart_fixture")) "OK restart requested" "coalesced restart"
    Wait-For {
        if (-not (Test-Path $pidFile)) { return $false }
        $current = @(Get-Content $pidFile | ForEach-Object { [int]$_ })
        return $current.Count -eq 2 -and $current[0] -ne $oldPids[0]
    } 8 "new generation pids"
    Assert-Contains (Invoke-Pm @("list")) "restart_fixture" "new generation remains managed"
    Assert-Processes-Gone $oldPids "old generation"
    Assert-Contains (Invoke-Pm @("stop", "restart_fixture")) "OK stop requested" "final restart stop"
    Wait-For-State "restart_fixture" "stopped" 8 "restart fixture stopped state"
    Stop-TestDaemon
}

function Run-CrashLoopScenario {
    Write-Host "[windows-integration] crash loop scenario"
    $work = Join-Path $ArtifactsDir "crash-loop"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    $command = "powershell.exe -NoProfile -Command Start-Sleep -Milliseconds 300; exit 1"
    $yaml = @"
- name: crash_loop
  command: $(Yaml-Quote $command)
  cwd: $(Yaml-Quote $work)
  daemon: true
  restart_delay_ms: 50
  restart_max_delay_ms: 50
  restart_window_ms: 10000
  restart_max_attempts: 2
  restart_reset_after_ms: 10000
"@
    Write-Utf8NoBom $config $yaml
    Start-TestDaemon "crash-loop" $config
    Wait-For {
        $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        $entry = @($status.processes | Where-Object { $_.name -eq "crash_loop" })
        return $entry.Count -eq 1 -and $entry[0].restart_count -eq 2 -and
            $entry[0].state -eq "stopped" -and $entry[0].restart_suppressed -and
            $entry[0].restart_attempts_in_window -eq 2
    } 10 "crash loop suppression"
    $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    $entry = @($status.processes | Where-Object { $_.name -eq "crash_loop" })[0]
    if ($entry.restart_suppression_reason -ne "restart attempt limit reached") {
        throw "crash loop suppression reason was not exposed"
    }
    $inspect = Invoke-Pm @("inspect", "crash_loop")
    Assert-Contains $inspect "restart_attempts_in_window=2" "crash loop inspect attempts"
    Assert-Contains $inspect "restart_suppressed=true" "crash loop inspect suppression"
    Assert-Contains $inspect "restart attempt limit reached" "crash loop inspect reason"
    $daemonLog = Get-Content (Join-Path $ArtifactsDir "crash-loop-daemon.stderr.log") -Raw
    Assert-Contains $daemonLog "automatic restart suppressed after 2 attempts" "crash loop log"

    Assert-Contains (Invoke-Pm @("start", "crash_loop")) "OK started" "manual recovery"
    Wait-For {
        $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        $entry = @($status.processes | Where-Object { $_.name -eq "crash_loop" })
        return $entry.Count -eq 1 -and $entry[0].restart_count -eq 5 -and
            $entry[0].state -eq "stopped" -and $entry[0].restart_suppressed -and
            $entry[0].restart_attempts_in_window -eq 2
    } 10 "manual restart policy reset"
    Stop-TestDaemon
}

try {
    Remove-Item -Recurse -Force $ArtifactsDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $ArtifactsDir | Out-Null
    Run-ConnectionFailureScenario
    Run-MainProtocolScenario
    Run-DependencyScenario
    Run-SdkScenario
    Run-StartTimeoutScenario
    Run-UnsupportedScenario
    Run-ProcessTreeCase "graceful" 3
    Run-ProcessTreeCase "resistant" 1
    Run-ProcessTreeCase "root-first" 1
    Run-RapidRestartScenario
    Run-CrashLoopScenario
    Write-Host "Windows protocol integration: PASS"
} catch {
    Write-Error $_
    exit 1
} finally {
    try { Stop-TestDaemon $false } catch { }
    $env:PM_TINY_PIPE_NAME = $script:PreviousPipeName
}
