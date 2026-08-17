param(
    [Parameter(Mandatory = $true)][string]$Daemon,
    [Parameter(Mandatory = $true)][string]$Cli,
    [Parameter(Mandatory = $true)][string]$SdkProbe,
    [Parameter(Mandatory = $true)][string]$ProtocolProbe,
    [Parameter(Mandatory = $true)][string]$TreeFixture,
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$ArtifactsDir,
    [string]$Scenario = "all"
)

$ErrorActionPreference = "Stop"
$script:ExpectedVersion = (Get-Content -LiteralPath (Join-Path $SourceDir "VERSION") -Raw).Trim()
$script:Utf8Encoding = [System.Text.UTF8Encoding]::new($false)
$OutputEncoding = $script:Utf8Encoding
[Console]::OutputEncoding = $script:Utf8Encoding
$env:PM_TINY_LOG_LEVEL = "debug"
$script:DaemonProcess = $null
$script:CurrentScenario = ""
$script:PreviousPipeName = $env:PM_TINY_PIPE_NAME
$script:PreviousPipeSddl = $env:PM_TINY_PIPE_SDDL
$script:PreviousHome = $env:PM_TINY_HOME
$script:PreviousProgramConfig = $env:PM_TINY_PROG_CFG_FILE
$script:PreviousAppLogDir = $env:PM_TINY_APP_LOG_DIR
$script:PreviousAppEnvironDir = $env:PM_TINY_APP_ENVIRON_DIR
$script:PreviousLogFile = $env:PM_TINY_LOG_FILE
$script:CurrentSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value

function Assert-Contains([string]$Actual, [string]$Expected, [string]$Context) {
    if (-not $Actual.Contains($Expected)) {
        throw "$Context did not contain '$Expected'. Actual: $Actual"
    }
}

function Assert-Empty([string]$Actual, [string]$Context) {
    if (-not [string]::IsNullOrWhiteSpace($Actual)) {
        throw "$Context expected empty output. Actual: $Actual"
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

function Invoke-PmExpectedFailure([string[]]$Arguments) {
    Write-Host "[windows-integration] pm $($Arguments -join ' ') (expect command failure)"
    $stdoutPath = Join-Path $ArtifactsDir "command-failure-cli.stdout.log"
    $stderrPath = Join-Path $ArtifactsDir "command-failure-cli.stderr.log"
    $process = Start-Process -FilePath $Cli -ArgumentList $Arguments `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru -Wait
    $stdout = Get-Content $stdoutPath -Raw -ErrorAction SilentlyContinue
    $stderr = Get-Content $stderrPath -Raw -ErrorAction SilentlyContinue
    if ($process.ExitCode -ne 1) {
        throw "pm $($Arguments -join ' ') expected exit code 1, got $($process.ExitCode). Output: $stdout$stderr"
    }
    return "$stdout$stderr"
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

function Set-TestEnvironmentPaths([string]$ConfigPath) {
    $work = Split-Path -Parent $ConfigPath
    $env:PM_TINY_HOME = $work
    $env:PM_TINY_PROG_CFG_FILE = Join-Path $work "prog.yaml"
    $env:PM_TINY_APP_LOG_DIR = Join-Path $work "logs"
    $env:PM_TINY_APP_ENVIRON_DIR = Join-Path $work "environ"
    $env:PM_TINY_LOG_FILE = Join-Path $work "pm_tiny.log"
}

function Start-TestDaemon([string]$Scenario, [string]$ConfigPath, [bool]$UseDefaultSddl = $false) {
    Write-Host "[windows-integration] starting $Scenario"
    $script:CurrentScenario = $Scenario
    $env:PM_TINY_PIPE_NAME = "\\.\pipe\pm_tiny_test_$PID`_$Scenario"
    if ($UseDefaultSddl) {
        $env:PM_TINY_PIPE_SDDL = $null
    } else {
        $env:PM_TINY_PIPE_SDDL = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;$script:CurrentSid)"
    }
    Set-TestEnvironmentPaths $ConfigPath
    $daemonConfig = Get-Content $ConfigPath | Where-Object { $_ -notmatch '^pm_tiny_pipe_name:' }
    $daemonConfig += "pm_tiny_pipe_name: '" + $env:PM_TINY_PIPE_NAME.Replace("'", "''") + "'"
    Write-Utf8NoBom $ConfigPath (($daemonConfig -join "`n") + "`n")
    $stdout = Join-Path $ArtifactsDir "$Scenario-daemon.stdout.log"
    $stderr = Join-Path $ArtifactsDir "$Scenario-daemon.stderr.log"
    $quotedConfigPath = '"' + $ConfigPath + '"'
    $script:DaemonProcess = Start-Process -FilePath $Daemon -ArgumentList @("--config", $quotedConfigPath) `
        -WorkingDirectory $ArtifactsDir -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    Wait-For {
        try { (Invoke-Pm @("version")).Contains("pm_tiny: $script:ExpectedVersion") } catch { $false }
    } 10 "$Scenario daemon control pipe"
}

function Run-InvalidSddlScenario {
    Write-Host "[windows-integration] invalid SDDL scenario"
    $work = Join-Path $ArtifactsDir "invalid-sddl"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    Set-TestEnvironmentPaths $config
    $stdout = Join-Path $work "daemon.stdout.log"
    $stderr = Join-Path $work "daemon.stderr.log"
    $previousSddl = $env:PM_TINY_PIPE_SDDL
    try {
        $env:PM_TINY_PIPE_SDDL = "not-an-sddl"
        $process = Start-Process -FilePath $Daemon -ArgumentList @("--config", ('"' + $config + '"')) `
            -WorkingDirectory $work -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -Wait
        if ($process.ExitCode -eq 0) { throw "daemon accepted invalid pipe SDDL" }
        Assert-Contains (Get-Content $stderr -Raw) "Invalid PM_TINY_PIPE_SDDL" "invalid SDDL error"
    } finally {
        $env:PM_TINY_PIPE_SDDL = $previousSddl
    }
}

function Run-DefaultSddlScenario {
    Write-Host "[windows-integration] default SDDL scenario"
    $work = Join-Path $ArtifactsDir "default-sddl"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    Start-TestDaemon "default-sddl" $config $true
    try {
        $info = Invoke-Pm @("info", "--json") | ConvertFrom-Json
        $expected = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)"
        if ($info.ipc.pipe_sddl.value -ne $expected -or $info.ipc.pipe_sddl.source -ne "default") {
            throw "default pipe SDDL mismatch: $($info.ipc.pipe_sddl | ConvertTo-Json -Compress)"
        }
    } finally {
        Stop-TestDaemon
    }
}

function Stop-TestDaemon([bool]$RequireCleanExit = $true, [int]$TimeoutMilliseconds = 10000) {
    if ($null -eq $script:DaemonProcess) { return }
    $script:DaemonProcess.Refresh()
    if (-not $script:DaemonProcess.HasExited) {
        try { [void](Invoke-Pm @("quit")) } catch { }
        $script:DaemonProcess.Refresh()
        if (-not $script:DaemonProcess.WaitForExit($TimeoutMilliseconds)) {
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

function Write-TestConfig([string]$ConfigPath, [string]$ProgramContent) {
    $programPath = Join-Path (Split-Path -Parent $ConfigPath) "prog.yaml"
    Write-Utf8NoBom $programPath $ProgramContent
    Write-Utf8NoBom $ConfigPath @"
pm_tiny_log_file: pm_tiny.log
pm_tiny_home_dir: .
pm_tiny_app_log_dir: logs
pm_tiny_app_environ_dir: environ
pm_tiny_prog_cfg_file: prog.yaml
pm_tiny_log_level: info
pm_tiny_log_max_size_kb: 4096
pm_tiny_log_archive_count: 3
"@
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
    Write-TestConfig $config "[]`n"
    Start-TestDaemon "connection-failure" $config
    Assert-ConnectionFailure "\\.\pipe\pm_tiny_wrong_$PID" "wrong pipe name"
    $configuredPipe = $env:PM_TINY_PIPE_NAME
    try {
        $env:PM_TINY_PIPE_NAME = $null
        Assert-Contains (Invoke-Pm @("version")) "pm_tiny: $script:ExpectedVersion" "pipe discovery through PM_TINY_HOME"
    } finally {
        $env:PM_TINY_PIPE_NAME = $configuredPipe
    }
    Stop-TestDaemon
}

function Yaml-Quote([string]$Value) {
    return "'" + $Value.Replace("'", "''") + "'"
}

function Expand-Fixture([string]$Name, [string]$Destination, [hashtable]$Values) {
    $content = Get-Content (Join-Path $SourceDir "tests/data/$Name") -Raw
    foreach ($entry in $Values.GetEnumerator()) {
        $content = $content.Replace("__$($entry.Key)__", $entry.Value)
    }
    Write-TestConfig $Destination $content
}

function Run-MainProtocolScenario {
    $work = Join-Path $ArtifactsDir "main"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $logDir = Join-Path $work "logs"
    $config = Join-Path $work "pm_tiny.yaml"
    $pingWithoutExtension = Join-Path $env:SystemRoot "System32\PING"
    $yaml = @"
- name: app
  executable: powershell.exe
  args: ["-NoProfile", "-Command", "Write-Output LOG_MARKER; Start-Sleep -Seconds 60"]
  cwd: $(Yaml-Quote $work)
  daemon: false
  log_dir: $(Yaml-Quote $logDir)
  log_file_name: app.log
- name: yaml_path_no_extension
  executable: $(Yaml-Quote $pingWithoutExtension)
  args: ["127.0.0.1", "-t"]
  cwd: $(Yaml-Quote $work)
  daemon: false
"@
    Write-TestConfig $config $yaml
    Start-TestDaemon "main" $config
    Write-Host "[windows-integration] main commands"

    Assert-Contains (Invoke-Pm @("version")) "pm_tiny: $script:ExpectedVersion" "version"
    $daemonInfo = Invoke-Pm @("info", "--json") | ConvertFrom-Json
    if ($daemonInfo.schema_version -ne 1 -or $daemonInfo.identity.platform -ne "windows") {
        throw "daemon info identity/schema mismatch"
    }
    if ($daemonInfo.runtime.mode -ne "foreground" -or
        $daemonInfo.process_tree.effective_mode -ne "job_object") {
        throw "daemon info foreground/process-tree mismatch"
    }
    if ($daemonInfo.ipc.named_pipe.value -ne $env:PM_TINY_PIPE_NAME -or
        $daemonInfo.capabilities.pty -ne $false -or
        $daemonInfo.capabilities.service_mode -ne $true) {
        throw "daemon info Windows configuration/capabilities mismatch"
    }
    Assert-Contains (Invoke-Pm @("list")) "app" "list"
    Wait-For-State "yaml_path_no_extension" "online" 10 "extensionless YAML executable"
    Assert-Contains (Invoke-Pm @("inspect", "yaml_path_no_extension")) $pingWithoutExtension `
        "YAML executable should retain its original value"
    Assert-Empty (Invoke-Pm @("stop", "yaml_path_no_extension", "--no-list")) `
        "extensionless YAML executable stop"
    $appInspect = Invoke-Pm @("inspect", "app")
    Assert-Contains $appInspect "name" "inspect name"
    Assert-Contains $appInspect "generation" "inspect generation"
    Assert-Contains $appInspect "job_object" "inspect process tree"
    Assert-Contains (Invoke-Pm @("restart", "app")) "app" "restart list"
    Assert-Contains (Invoke-Pm @("stop", "app")) "app" "stop list"
    Wait-For-State "app" "stopped" 10 "app stopped state"

    Assert-Contains (Invoke-Pm @("start", "app")) 'started `app` pid=' "start for generation log"
    $logOutput = Invoke-Pm @("log", "app")
    Assert-Contains (Invoke-Pm @("stop", "app")) "app" "stop after generation log"
    Assert-Contains $logOutput "LOG_MARKER" "streamed log"
    Assert-Empty (Invoke-Pm @("save")) "save"
    if (Test-Path (Join-Path $work "prog.yaml.tmp")) { throw "transactional save left a temporary file" }
    Assert-Contains (Invoke-Pm @("reload")) "app" "reload list"
    Assert-Contains (Invoke-Pm @("delete", "app")) "Total:" "delete list"
    Assert-Contains (Invoke-PmExpectedFailure @("inspect", "app")) "process not found" "missing inspect"
    Assert-Contains (Invoke-PmExpectedFailure @("stop", "missing")) "process not found" "missing stop"
    Assert-Contains (Invoke-PmExpectedFailure @("start", "missing")) "process not found" "missing start definition"
    Assert-Empty (Invoke-Pm @("save")) "empty save"
    Assert-Contains (Invoke-Pm @("reload")) "Total:" "empty reload"

    foreach ($mode in @("fragmented", "coalesced", "unknown-type", "invalid-magic", "invalid-version", "invalid-flags", "oversize", "slow")) {
        & $ProtocolProbe $mode
        if ($LASTEXITCODE -ne 0) { throw "protocol probe failed: $mode" }
        Assert-Contains (Invoke-Pm @("version")) "pm_tiny: $script:ExpectedVersion" "daemon recovery after $mode"
    }
    Stop-TestDaemon
}

function Run-DynamicStartScenario {
    Write-Host "[windows-integration] dynamic start scenario"
    $work = Join-Path $ArtifactsDir "dynamic-start"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    Start-TestDaemon "dynamic-start" $config
    $previousMarker = $env:V3_TEST_MARKER
    try {
        $env:V3_TEST_MARKER = "inherited"
        $missingCwd = Join-Path $work "missing-cwd"
        Assert-Contains (Invoke-PmFailure @("start", "missing-cwd", "--cwd", $missingCwd,
            "--", "powershell.exe")) "directory not found" "missing cwd validation"
        $cwdFile = Join-Path $work "cwd-file"
        Write-Utf8NoBom $cwdFile "not a directory"
        Assert-Contains (Invoke-PmFailure @("start", "file-cwd", "--cwd", $cwdFile,
            "--", "powershell.exe")) "not a directory" "cwd file validation"
        $expectedCwd = [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path "."))
        $output = Invoke-Pm @("start", "dynamic", "--cwd", ".", "--no-daemon",
            "--env", "APP_MARKER=explicit", "--", "powershell", "-NoProfile",
            "-Command", "`$args | Out-Null; Write-Output (`$env:V3_TEST_MARKER + ':' + `$env:APP_MARKER); Start-Sleep -Seconds 60",
            "hello world", "")
        Assert-Contains $output 'started `dynamic` pid=' "dynamic start"
        Assert-Contains $output "pm save" "dynamic persistence hint"
        $inspect = Invoke-Pm @("inspect", "dynamic")
        Assert-Contains $inspect "powershell.exe" "dynamic executable"
        Assert-Contains $inspect "hello world" "dynamic structured arguments"
        Assert-Contains $inspect "runtime" "dynamic config source"
        Assert-Contains $inspect $expectedCwd "dynamic absolute cwd"
        Assert-Contains $inspect (Join-Path $work "logs") "dynamic default log directory"
        Assert-Contains $inspect "unsupported" "Windows PTY capability"
        $wide = Invoke-Pm @("list", "--wide", "--no-color")
        if ($wide.Contains([string][char]0x9205) -or $wide.Contains([string][char]0xfffd)) {
            throw "structured Windows CLI output was decoded with a non-UTF8 code page: $wide"
        }
        Wait-For {
            try { (Invoke-Pm @("log", "dynamic")).Contains("inherited:explicit") } catch { $false }
        } 10 "dynamic inherited and explicit environment"

        Assert-Empty (Invoke-Pm @("save")) "dynamic save"
        $saved = Get-Content (Join-Path $work "prog.yaml") -Raw
        Assert-Contains $saved "APP_MARKER=explicit" "saved explicit environment"
        if ($saved.Contains("inherited_env:")) { throw "prog.yaml retained removed inherited_env field" }
        $sidecar = Get-Content (Join-Path $work "environ\dynamic.yaml") -Raw
        Assert-Contains $sidecar "schema: 1" "saved environment sidecar schema"
        Assert-Contains $sidecar "V3_TEST_MARKER=inherited" "saved inherited environment sidecar"
        Assert-Contains (Invoke-PmExpectedFailure @("start", "dynamic", "--", "powershell.exe")) `
            "process already exists" "dynamic name conflict"
        Assert-Contains (Invoke-PmExpectedFailure @("start", "broken", "--", "Z:\missing\pm_tiny.exe")) `
            "resolve: executable not found" "client executable resolution failure"
        Assert-Contains (Invoke-PmExpectedFailure @("inspect", "broken")) "process not found" `
            "CreateProcess rollback"

        $short = Invoke-Pm @("start", "short-lived", "--no-daemon", "--", "powershell.exe",
            "-NoProfile", "-Command", "exit 0")
        Assert-Contains $short 'started `short-lived` pid=' "short-lived no-daemon start"
        Wait-For-State "short-lived" "stopped" 10 "no-daemon process exit"
        $shortStatus = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        $shortEntry = @($shortStatus.processes | Where-Object { $_.name -eq "short-lived" })[0]
        if ($shortEntry.restart_pending -eq $true -or $shortEntry.state -ne "stopped") {
            throw "--no-daemon process retained an automatic restart: $($shortEntry | ConvertTo-Json -Compress)"
        }
        $shortGeneration = $shortEntry.generation
        Start-Sleep -Seconds 2
        $shortLater = (Invoke-Pm @("list", "--json") | ConvertFrom-Json).processes |
            Where-Object { $_.name -eq "short-lived" }
        if ($shortLater.generation -ne $shortGeneration -or $shortLater.restart_pending -eq $true) {
            throw "--no-daemon process was relaunched: $($shortLater | ConvertTo-Json -Compress)"
        }

        $unicodeName = [string][char]0x7a0b + [string][char]0x5e8f
        Assert-Contains (Invoke-Pm @("start", $unicodeName, "--no-daemon", "--", "powershell.exe",
            "-NoProfile", "-Command", "Write-Output unicode-log; exit 0")) `
            "started" "Unicode process start"
        Wait-For-State $unicodeName "stopped" 10 "Unicode process exit"
        $unicodeHistory = Invoke-Pm @("log", $unicodeName, "--history")
        Assert-Contains $unicodeHistory "showing cached log for stopped process ``$unicodeName``" `
            "Unicode history header"
        Assert-Contains $unicodeHistory "unicode-log" "Unicode history content"
        if ($unicodeHistory.Contains("process ``$unicodeName`` exited")) {
            throw "Unicode history contained a synthetic realtime exit event"
        }

        $systemPing = Join-Path $env:SystemRoot "System32\PING"
        $resolved = Invoke-Pm @("start", "resolved-path", "--no-daemon", "--", $systemPing, "127.0.0.1", "-n", "1")
        Assert-Contains $resolved 'started `resolved-path` pid=' "client executable resolution"
        Assert-Contains (Invoke-Pm @("inspect", "resolved-path")) ".exe" "resolved executable extension"
        Assert-Empty (Invoke-Pm @("stop", "resolved-path", "--no-list")) "resolved path stop"
    } finally {
        $env:V3_TEST_MARKER = $previousMarker
        Stop-TestDaemon
    }
}

function Run-EmptyProgramConfigScenario {
    Write-Host "[windows-integration] empty program config scenario"
    $work = Join-Path $ArtifactsDir "empty-program-config"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    $programPath = Join-Path $work "prog.yaml"
    Remove-Item $programPath

    Start-TestDaemon "missing-program-config" $config
    if (Test-Path $programPath) { throw "daemon created missing prog.yaml during startup" }
    $info = Invoke-Pm @("info", "--json") | ConvertFrom-Json
    if ($info.runtime.file_config_count -ne 0 -or $info.runtime.runtime_definition_count -ne 0) {
        throw "missing program config did not start with zero definitions"
    }
    $list = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    if ($list.total -ne 0 -or @($list.processes).Count -ne 0) {
        throw "missing program config did not produce an empty process list"
    }
    Assert-Contains (Invoke-Pm @("start", "empty_probe", "--no-daemon", "--",
        "powershell.exe", "-NoProfile", "-Command", "Start-Sleep -Seconds 60")) `
        'started `empty_probe` pid=' "dynamic start from missing config"
    Assert-Empty (Invoke-Pm @("save")) "save after missing config"
    if (-not (Test-Path $programPath)) { throw "pm save did not create prog.yaml" }
    Assert-Contains (Get-Content $programPath -Raw) "name: empty_probe" "saved dynamic definition"

    Write-Utf8NoBom $programPath "  # intentionally empty for reload`n"
    Assert-Contains (Invoke-Pm @("reload")) "Total:" "comment-only reload"
    $list = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    if ($list.total -ne 0 -or @($list.processes).Count -ne 0) {
        throw "comment-only reload did not clear definitions"
    }
    Stop-TestDaemon

    Write-Utf8NoBom $programPath ""
    Start-TestDaemon "zero-byte-program-config" $config
    $list = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    if ($list.total -ne 0 -or @($list.processes).Count -ne 0) {
        throw "zero-byte program config did not produce an empty process list"
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

function Run-LifecycleTransitionScenario {
    Write-Host "[windows-integration] lifecycle transition scenario"
    $work = Join-Path $ArtifactsDir "lifecycle-transitions"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    Start-TestDaemon "lifecycle-transitions" $config

    Assert-Contains (Invoke-Pm @("start", "lifecycle-peer", "--no-daemon", "--",
        "powershell.exe", "-NoProfile", "-Command", "Start-Sleep -Seconds 60")) `
        'started `lifecycle-peer` pid=' "lifecycle peer start"
    Assert-Contains (Invoke-Pm @("start", "lifecycle-target", "--no-daemon", "--",
        "powershell.exe", "-NoProfile", "-Command", "Start-Sleep -Seconds 60")) `
        'started `lifecycle-target` pid=' "lifecycle target start"
    Wait-For-State "lifecycle-peer" "online" 10 "lifecycle peer online"
    Wait-For-State "lifecycle-target" "online" 10 "lifecycle target online"
    $before = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    $peerBefore = @($before.processes | Where-Object { $_.name -eq "lifecycle-peer" })[0]
    $targetBefore = @($before.processes | Where-Object { $_.name -eq "lifecycle-target" })[0]

    Assert-Empty (Invoke-Pm @("stop", "lifecycle-target", "--no-list")) `
        "lifecycle target stop"
    Wait-For-State "lifecycle-target" "stopped" 10 "lifecycle target stopped"
    Assert-Contains (Invoke-Pm @("start", "lifecycle-target")) 'started `lifecycle-target` pid=' `
        "lifecycle target restart with online peer"
    Wait-For-State "lifecycle-target" "online" 10 "lifecycle target restarted"
    $after = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    $peerAfter = @($after.processes | Where-Object { $_.name -eq "lifecycle-peer" })[0]
    $targetAfter = @($after.processes | Where-Object { $_.name -eq "lifecycle-target" })[0]
    if ($peerAfter.state -ne "online" -or $peerAfter.pid -ne $peerBefore.pid -or
        $peerAfter.generation -ne $peerBefore.generation -or
        $targetAfter.generation -le $targetBefore.generation) {
        throw "stop/start changed the online peer or failed to advance target generation"
    }

    Assert-Contains (Invoke-Pm @("start", "startup-probe", "--no-daemon", "--start-timeout", "-1",
        "--", "powershell.exe", "-NoProfile", "-Command", "Start-Sleep -Seconds 60")) `
        'started `startup-probe` pid=' "starting-state probe start"
    Wait-For-State "startup-probe" "starting" 10 "starting-state probe starting"
    $startupBefore = (Invoke-Pm @("list", "--json") | ConvertFrom-Json).processes |
        Where-Object { $_.name -eq "startup-probe" }
    Assert-Empty (Invoke-Pm @("stop", "startup-probe", "--no-list")) `
        "starting-state probe stop"
    Wait-For-State "startup-probe" "stopped" 10 "starting-state probe stopped"
    for ($index = 0; $index -lt 10; $index++) {
        $stopped = (Invoke-Pm @("list", "--json") | ConvertFrom-Json).processes |
            Where-Object { $_.name -eq "startup-probe" }
        if ($stopped.state -ne "stopped" -or $null -ne $stopped.pid -or
            $stopped.generation -ne $startupBefore.generation) {
            throw "stopped starting-state probe was relaunched"
        }
        Start-Sleep -Milliseconds 50
    }

    Assert-Contains (Invoke-Pm @("start", "startup-probe")) 'started `startup-probe` pid=' `
        "starting-state probe manual start"
    Wait-For-State "startup-probe" "starting" 10 "starting-state probe restarted"
    $startupRestarted = (Invoke-Pm @("list", "--json") | ConvertFrom-Json).processes |
        Where-Object { $_.name -eq "startup-probe" }
    if ($startupRestarted.generation -le $startupBefore.generation) {
        throw "manual start did not advance starting-state probe generation"
    }
    Assert-Empty (Invoke-Pm @("restart", "startup-probe", "--no-list")) `
        "starting-state probe restart"
    Wait-For {
        $item = (Invoke-Pm @("list", "--json") | ConvertFrom-Json).processes |
            Where-Object { $_.name -eq "startup-probe" }
        $item.state -eq "starting" -and $item.generation -gt $startupRestarted.generation
    } 10 "starting-state probe new restart generation"

    Assert-Empty (Invoke-Pm @("delete", "startup-probe", "--no-list")) `
        "starting-state probe delete"
    $remaining = (Invoke-Pm @("list", "--json") | ConvertFrom-Json).processes
    if (@($remaining | Where-Object { $_.name -eq "startup-probe" }).Count -ne 0) {
        throw "deleted starting-state probe remained in process list"
    }
    Assert-Empty (Invoke-Pm @("delete", "lifecycle-target", "--no-list")) `
        "lifecycle target delete"
    Assert-Empty (Invoke-Pm @("delete", "lifecycle-peer", "--no-list")) `
        "lifecycle peer delete"
    Stop-TestDaemon
}

function Run-DynamicDependencyDeleteScenario {
    Write-Host "[windows-integration] dynamic dependency delete scenario"
    $work = Join-Path $ArtifactsDir "dynamic-dependency-delete"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    Start-TestDaemon "dynamic-dependency-delete" $config

    Assert-Contains (Invoke-Pm @("start", "dependency-base", "--no-daemon", "--",
        "ping.exe", "-t", "127.0.0.1")) 'started `dependency-base` pid=' `
        "dynamic dependency base start"
    Assert-Contains (Invoke-Pm @("start", "dependency-child", "--no-daemon", "--depends-on",
        "dependency-base", "--", "ping.exe", "-t", "127.0.0.1")) `
        'started `dependency-child` pid=' "dynamic dependency child start"
    Wait-For-State "dependency-base" "online" 10 "dynamic dependency base online"
    Wait-For-State "dependency-child" "online" 10 "dynamic dependency child online"

    Assert-Empty (Invoke-Pm @("save")) "dynamic dependency save"
    $programPath = Join-Path $work "prog.yaml"
    $savedBeforeDelete = Get-Content $programPath -Raw

    $beforeInvalidReload = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    $beforeInvalidBase = @($beforeInvalidReload.processes |
        Where-Object { $_.name -eq "dependency-base" })[0]
    $beforeInvalidChild = @($beforeInvalidReload.processes |
        Where-Object { $_.name -eq "dependency-child" })[0]
    $beforeInvalidGraph = Invoke-Pm @("graph", "dependency-base", "--json") |
        ConvertFrom-Json | ConvertTo-Json -Depth 10 -Compress
    $invalidProgram = @"
- name: dependency-child
  executable: powershell.exe
  args: ["-NoProfile", "-Command", "Start-Sleep", "-Seconds", "60"]
  cwd: $(Yaml-Quote $work)
  daemon: false
  depends_on: [missing]
- name: dependency-base
  executable: powershell.exe
  args: ["-NoProfile", "-Command", "Start-Sleep", "-Seconds", "60"]
  cwd: $(Yaml-Quote $work)
  daemon: false
"@
    Write-Utf8NoBom $programPath $invalidProgram
    $invalidReloadError = Invoke-PmExpectedFailure @("reload", "--no-list")
    if (-not ($invalidReloadError.Contains("missing") -or
            $invalidReloadError.Contains("cannot load program configuration"))) {
        throw "invalid reload did not report dependency/configuration validation: $invalidReloadError"
    }
    $afterInvalidReload = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    $afterInvalidBase = @($afterInvalidReload.processes |
        Where-Object { $_.name -eq "dependency-base" })[0]
    $afterInvalidChild = @($afterInvalidReload.processes |
        Where-Object { $_.name -eq "dependency-child" })[0]
    if ($afterInvalidBase.state -ne $beforeInvalidBase.state -or
        $afterInvalidBase.pid -ne $beforeInvalidBase.pid -or
        $afterInvalidBase.generation -ne $beforeInvalidBase.generation -or
        $afterInvalidChild.state -ne $beforeInvalidChild.state -or
        $afterInvalidChild.pid -ne $beforeInvalidChild.pid -or
        $afterInvalidChild.generation -ne $beforeInvalidChild.generation -or
        (@($afterInvalidChild.depends_on) -join ",") -ne (@($beforeInvalidChild.depends_on) -join ",")) {
        throw "invalid reload changed runtime state: $($afterInvalidReload | ConvertTo-Json -Depth 10 -Compress)"
    }
    $afterInvalidGraph = Invoke-Pm @("graph", "dependency-base", "--json") |
        ConvertFrom-Json | ConvertTo-Json -Depth 10 -Compress
    if ($afterInvalidGraph -ne $beforeInvalidGraph) {
        throw "invalid reload changed dependency graph"
    }
    Write-Utf8NoBom $programPath $savedBeforeDelete
    if ((Get-Content $programPath -Raw) -ne $savedBeforeDelete) {
        throw "invalid reload test did not restore persisted configuration"
    }

    $deleteError = Invoke-PmExpectedFailure @("delete", "dependency-base", "--no-list")
    Assert-Contains $deleteError "cannot delete ``dependency-base``; required by: dependency-child" `
        "running dependency delete rejection"

    $list = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    $base = @($list.processes | Where-Object { $_.name -eq "dependency-base" })[0]
    $child = @($list.processes | Where-Object { $_.name -eq "dependency-child" })[0]
    if ($base.state -ne "online" -or $child.state -ne "online" -or
        @($child.depends_on).Count -ne 1 -or $child.depends_on[0] -ne "dependency-base") {
        throw "rejected dependency delete changed runtime state: $($list | ConvertTo-Json -Depth 8 -Compress)"
    }
    $graph = Invoke-Pm @("graph", "dependency-base", "--json") | ConvertFrom-Json
    if (@($graph.nodes).Count -ne 2 -or @($graph.edges).Count -ne 1 -or
        $graph.edges[0].from -ne "dependency-base" -or $graph.edges[0].to -ne "dependency-child") {
        throw "rejected dependency delete changed graph: $($graph | ConvertTo-Json -Depth 8 -Compress)"
    }
    if ((Get-Content $programPath -Raw) -ne $savedBeforeDelete) {
        throw "rejected dependency delete changed persisted configuration"
    }

    Assert-Empty (Invoke-Pm @("stop", "dependency-child", "--no-list")) `
        "dependency child stop after rejected delete"
    Wait-For-State "dependency-child" "stopped" 10 "dependency child stopped after rejected delete"
    Assert-Contains (Invoke-Pm @("start", "dependency-child")) 'started `dependency-child` pid=' `
        "dependency child restart after rejected delete"
    Wait-For-State "dependency-child" "online" 10 "dependency child restarted after rejected delete"

    Assert-Empty (Invoke-Pm @("stop", "dependency-base", "--no-list")) `
        "dependency base stop before stopped delete rejection"
    Wait-For-State "dependency-base" "stopped" 10 "dependency base stopped before delete rejection"
    $stoppedDeleteError = Invoke-PmExpectedFailure @("delete", "dependency-base", "--no-list")
    Assert-Contains $stoppedDeleteError "cannot delete ``dependency-base``; required by: dependency-child" `
        "stopped dependency delete rejection"

    Assert-Empty (Invoke-Pm @("delete", "dependency-child", "--no-list")) `
        "dependency leaf delete"
    Assert-Empty (Invoke-Pm @("delete", "dependency-base", "--no-list")) `
        "dependency base delete after leaf"
    Assert-Empty (Invoke-Pm @("save")) "save after valid dependency deletes"
    Assert-Contains (Invoke-Pm @("reload")) "Total: 0" `
        "reload after valid dependency deletes"
    $finalList = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    if ($finalList.total -ne 0 -or @($finalList.processes).Count -ne 0) {
        throw "valid dependency deletes did not empty runtime definitions"
    }
    $savedAfterDelete = Get-Content $programPath -Raw
    if ($savedAfterDelete.Contains("dependency-base") -or $savedAfterDelete.Contains("dependency-child")) {
        throw "save after valid dependency deletes retained deleted definitions"
    }
    Stop-TestDaemon
}

function Start-ConcurrentPm([string]$Label, [string[]]$Arguments) {
    $stdoutPath = Join-Path $ArtifactsDir "concurrent-$Label.stdout.log"
    $stderrPath = Join-Path $ArtifactsDir "concurrent-$Label.stderr.log"
    return Start-Process -FilePath $Cli -ArgumentList $Arguments `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
}

function Wait-ConcurrentPm([object[]]$Processes, [string]$Context) {
    foreach ($process in $Processes) {
        $process.WaitForExit()
        if ($process.ExitCode -gt 1) {
            throw "$Context command exited with unexpected code $($process.ExitCode)"
        }
    }
}

function Run-ConcurrentControlScenario {
    Write-Host "[windows-integration] concurrent control scenario"
    $work = Join-Path $ArtifactsDir "concurrent-controls"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    Start-TestDaemon "concurrent-controls" $config

    Assert-Contains (Invoke-Pm @("start", "concurrent-worker", "--no-daemon", "--",
        "powershell.exe", "-NoProfile", "-Command", "Start-Sleep -Seconds 60")) `
        'started `concurrent-worker` pid=' "concurrent worker start"
    Assert-Contains (Invoke-Pm @("start", "concurrent-delete-target", "--no-daemon", "--",
        "powershell.exe", "-NoProfile", "-Command", "Start-Sleep -Seconds 60")) `
        'started `concurrent-delete-target` pid=' "concurrent delete target start"
    Wait-For-State "concurrent-worker" "online" 10 "concurrent worker online"
    Wait-For-State "concurrent-delete-target" "online" 10 "concurrent delete target online"

    $wave = @()
    for ($index = 1; $index -le 8; $index++) {
        $wave += Start-ConcurrentPm "wave1-list-$index" @("list", "--json")
    }
    for ($index = 1; $index -le 4; $index++) {
        $wave += Start-ConcurrentPm "wave1-inspect-$index" @("inspect", "concurrent-worker")
        $wave += Start-ConcurrentPm "wave1-save-$index" @("save")
        $wave += Start-ConcurrentPm "wave1-reload-$index" @("reload", "--no-list")
    }
    Wait-ConcurrentPm $wave "wave1"
    Assert-Contains (Invoke-Pm @("version")) "pm_tiny: $script:ExpectedVersion" "daemon after concurrent read/persistence wave"

    $wave = @()
    for ($index = 1; $index -le 4; $index++) {
        $wave += Start-ConcurrentPm "wave2-stop-$index" @("stop", "concurrent-worker", "--no-list")
        $wave += Start-ConcurrentPm "wave2-start-$index" @("start", "concurrent-worker")
        $wave += Start-ConcurrentPm "wave2-restart-$index" @("restart", "concurrent-worker", "--no-list")
        $wave += Start-ConcurrentPm "wave2-delete-$index" @("delete", "concurrent-delete-target", "--no-list")
        $wave += Start-ConcurrentPm "wave2-save-$index" @("save")
        $wave += Start-ConcurrentPm "wave2-reload-$index" @("reload", "--no-list")
    }
    Wait-ConcurrentPm $wave "wave2"
    $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    if ($status.schema_version -ne 5 -or $null -eq $status.processes) {
        throw "daemon returned invalid process list after concurrent control wave"
    }
    Assert-Contains (Invoke-Pm @("version")) "pm_tiny: $script:ExpectedVersion" "daemon after concurrent lifecycle wave"
    Stop-TestDaemon
}

function Run-WaitingStartLogScenario {
    Write-Host "[windows-integration] waiting start --log scenario"
    $work = Join-Path $ArtifactsDir "waiting-start-log"
    $logDir = Join-Path $work "logs"
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    $yaml = @"
- name: ready_base
  executable: $(Yaml-Quote $SdkProbe)
  cwd: $(Yaml-Quote $work)
  daemon: false
  start_timeout: 2
  log_dir: $(Yaml-Quote $logDir)
- name: log_child
  executable: powershell.exe
  args: ["-NoProfile", "-Command", "Write-Output WAITING_LOG_MARKER; Start-Sleep -Seconds 1"]
  cwd: $(Yaml-Quote $work)
  daemon: false
  depends_on: [ready_base]
  log_dir: $(Yaml-Quote $logDir)
"@
    Write-TestConfig $config $yaml
    Start-TestDaemon "waiting-start-log" $config
    Wait-For-State "log_child" "stopped" 10 "initial child exit"
    $output = Invoke-Pm @("start", "log_child", "--log")
    Assert-Contains $output "WAITING_LOG_MARKER" "waiting start log stream"
    Stop-TestDaemon
}

function Run-LogHistoryAndRestartGenerationScenario {
    Write-Host "[windows-integration] log history and restart generation scenario"
    $work = Join-Path $ArtifactsDir "log-history-restart-generation"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    Start-TestDaemon "log-history-restart-generation" $config

    Invoke-Pm @("start", "log_history", "--no-daemon", "--", "powershell.exe",
        "-NoProfile", "-Command", "Write-Output HISTORY_MARKER") | Out-Null
    Wait-For-State "log_history" "stopped" 10 "history process stopped"
    $liveError = Invoke-PmFailure @("log", "log_history")
    Assert-Contains $liveError "--history" "stopped live log hint"
    $history = Invoke-Pm @("log", "log_history", "--history")
    Assert-Contains $history "showing cached log for stopped process ``log_history``" "history header"
    Assert-Contains $history "HISTORY_MARKER" "history content"
    if ($history.Contains("process ``log_history`` exited")) {
        throw "history replay contained a synthetic realtime exit event"
    }
    $historySecond = Invoke-Pm @("log", "log_history", "--history")
    if ($historySecond -ne $history) { throw "repeated history replay changed without a new generation" }
    Invoke-Pm @("delete", "log_history", "--no-list") | Out-Null

    $counter = Join-Path $work "generation.txt"
    $scriptPath = Join-Path $work "generation.ps1"
    Write-Utf8NoBom $scriptPath @'
param([string]$Counter)
$generation = if (Test-Path $Counter) { [int](Get-Content $Counter -Raw) + 1 } else { 1 }
[IO.File]::WriteAllText($Counter, [string]$generation)
Write-Output "GENERATION_$generation"
if ($generation -eq 1) { Start-Sleep -Seconds 30 } else { Start-Sleep -Milliseconds 100 }
'@
    Invoke-Pm @("start", "restart_log_generation", "--no-daemon", "--", "powershell.exe",
        "-NoProfile", "-File", $scriptPath, $counter) | Out-Null
    Wait-For { (Test-Path $counter) -and ((Get-Content $counter -Raw).Trim() -eq "1") } 10 "first generation marker"
    $restartOutput = Invoke-Pm @("restart", "restart_log_generation", "--log")
    Assert-Contains $restartOutput "GENERATION_2" "new restart generation log"
    if ($restartOutput.Contains("GENERATION_1")) { throw "restart --log replayed the old generation" }
    Assert-Contains $restartOutput "process ``restart_log_generation`` exited" "new generation exit event"
    Invoke-Pm @("delete", "restart_log_generation", "--no-list") | Out-Null

    $automaticCounter = Join-Path $work "automatic-generation.txt"
    $automaticScript = Join-Path $work "automatic-generation.ps1"
    Write-Utf8NoBom $automaticScript @'
param([string]$Counter)
$generation = if (Test-Path $Counter) { [int](Get-Content $Counter -Raw) + 1 } else { 1 }
[IO.File]::WriteAllText($Counter, [string]$generation)
Write-Output "AUTOMATIC_GENERATION_$generation"
Start-Sleep -Milliseconds 100
'@
    Invoke-Pm @("start", "automatic_log_wait", "--restart-delay-ms", "1500",
        "--restart-max-delay-ms", "1500", "--", "powershell.exe", "-NoProfile", "-File",
        $automaticScript, $automaticCounter) | Out-Null
    Wait-For-State "automatic_log_wait" "waiting" 10 "automatic restart waiting state"
    $automaticOutput = Invoke-Pm @("log", "automatic_log_wait")
    Assert-Contains $automaticOutput "AUTOMATIC_GENERATION_2" "next automatic generation log"
    if ($automaticOutput.Contains("AUTOMATIC_GENERATION_1")) {
        throw "waiting log replayed the completed automatic generation"
    }
    Assert-Contains $automaticOutput "process ``automatic_log_wait`` exited" `
        "next automatic generation exit event"
    Invoke-Pm @("delete", "automatic_log_wait", "--no-list") | Out-Null
    Stop-TestDaemon
}

function Run-SdkScenario {
    Write-Host "[windows-integration] sdk scenario"
    $work = Join-Path $ArtifactsDir "sdk"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Expand-Fixture "windows_sdk_managed.yaml" $config @{
        SDK_PROBE = $SdkProbe
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
    Set-TestEnvironmentPaths $config
    $stdout = Join-Path $ArtifactsDir "unsupported-daemon.stdout.log"
    $stderr = Join-Path $ArtifactsDir "unsupported-daemon.stderr.log"
    $process = Start-Process -FilePath $Daemon -ArgumentList @("--config", ('"' + $config + '"')) `
        -WorkingDirectory $ArtifactsDir -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -Wait
    if ($process.ExitCode -eq 0) { throw "unsupported configuration unexpectedly started daemon" }
    $daemonLog = Get-Content (Join-Path $ArtifactsDir "unsupported-daemon.stderr.log") -Raw
    Assert-Contains $daemonLog "pty: true" "unsupported Windows configuration"

    Write-TestConfig $config @"
- name: removed_environment
  executable: cmd.exe
  args: ["/c", "exit", "0"]
  cwd: $(Yaml-Quote $work)
  inherited_env: [VALUE=old]
"@
    Set-TestEnvironmentPaths $config
    $removedStdout = Join-Path $ArtifactsDir "removed-environment.stdout.log"
    $removedStderr = Join-Path $ArtifactsDir "removed-environment.stderr.log"
    $removed = Start-Process -FilePath $Daemon -ArgumentList @("--config", ('"' + $config + '"')) `
        -WorkingDirectory $ArtifactsDir -RedirectStandardOutput $removedStdout `
        -RedirectStandardError $removedStderr -PassThru -Wait
    if ($removed.ExitCode -eq 0) { throw "removed inherited_env configuration unexpectedly started daemon" }
    Assert-Contains (Get-Content $removedStderr -Raw) "removed field ``inherited_env``" `
        "removed inherited environment format"
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
  executable: $(Yaml-Quote $TreeFixture)
  args: ["--mode", "$Mode", "--pid-file", $(Yaml-Quote $pidFile), "--marker-file", $(Yaml-Quote $markerFile)]
  cwd: $(Yaml-Quote $work)
  daemon: false
  kill_timeout: $KillTimeout
"@
    Write-TestConfig $config $yaml
    Start-TestDaemon "tree-$Mode" $config
    Wait-For { (Test-Path $pidFile) -and ((Get-Content $pidFile).Count -eq 2) } 5 "$Mode pid file"
    $processIds = @(Get-Content $pidFile | ForEach-Object { [int]$_ })
    $started = [DateTime]::UtcNow
    Assert-Contains (Invoke-Pm @("stop", "tree_fixture")) "tree_fixture" "$Mode stop list"
    $versionStarted = [DateTime]::UtcNow
    Assert-Contains (Invoke-Pm @("version")) "pm_tiny: $script:ExpectedVersion" "$Mode version during termination"
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
  executable: $(Yaml-Quote $TreeFixture)
  args: ["--mode", "resistant", "--pid-file", $(Yaml-Quote $pidFile), "--marker-file", $(Yaml-Quote $markerFile)]
  cwd: $(Yaml-Quote $work)
  daemon: false
  kill_timeout: 1
"@
    Write-TestConfig $config $yaml
    Start-TestDaemon "rapid-restart" $config
    Wait-For { (Test-Path $pidFile) -and ((Get-Content $pidFile).Count -eq 2) } 5 "initial restart pids"
    $oldPids = @(Get-Content $pidFile | ForEach-Object { [int]$_ })
    Assert-Contains (Invoke-Pm @("restart", "restart_fixture")) "restart_fixture" "first restart list"
    Assert-Contains (Invoke-Pm @("restart", "restart_fixture")) "restart_fixture" "coalesced restart list"
    Wait-For {
        if (-not (Test-Path $pidFile)) { return $false }
        $current = @(Get-Content $pidFile | ForEach-Object { [int]$_ })
        return $current.Count -eq 2 -and $current[0] -ne $oldPids[0]
    } 8 "new generation pids"
    Assert-Contains (Invoke-Pm @("list")) "restart_fixture" "new generation remains managed"
    Assert-Processes-Gone $oldPids "old generation"
    Assert-Contains (Invoke-Pm @("stop", "restart_fixture")) "restart_fixture" "final restart stop list"
    Wait-For-State "restart_fixture" "stopped" 8 "restart fixture stopped state"
    Stop-TestDaemon
}

function Run-CrashLoopScenario {
    Write-Host "[windows-integration] crash loop scenario"
    $work = Join-Path $ArtifactsDir "crash-loop"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    $yaml = @"
- name: crash_loop
  executable: powershell.exe
  args: ["-NoProfile", "-Command", "Start-Sleep -Milliseconds 300; exit 1"]
  cwd: $(Yaml-Quote $work)
  daemon: true
  restart_delay_ms: 50
  restart_max_delay_ms: 50
  restart_window_ms: 10000
  restart_max_attempts: 2
  restart_reset_after_ms: 10000
"@
    Write-TestConfig $config $yaml
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
    Assert-Contains $inspect "restart_attempts_in_window" "crash loop inspect attempts field"
    Assert-Contains $inspect "2" "crash loop inspect attempts"
    Assert-Contains $inspect "restart_suppressed" "crash loop inspect suppression field"
    Assert-Contains $inspect "Y" "crash loop inspect suppression"
    Assert-Contains $inspect "restart attempt limit reached" "crash loop inspect reason"
    $daemonLog = Get-Content (Join-Path $ArtifactsDir "crash-loop-daemon.stderr.log") -Raw
    Assert-Contains $daemonLog "automatic restart suppressed after 2 attempts" "crash loop log"

    Assert-Contains (Invoke-Pm @("start", "crash_loop")) 'started `crash_loop` pid=' "manual recovery"
    Wait-For {
        $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        $entry = @($status.processes | Where-Object { $_.name -eq "crash_loop" })
        return $entry.Count -eq 1 -and $entry[0].restart_count -eq 5 -and
            $entry[0].state -eq "stopped" -and $entry[0].restart_suppressed -and
            $entry[0].restart_attempts_in_window -eq 2
    } 10 "manual restart policy reset"
    Stop-TestDaemon
}

function Run-StopPendingRestartScenario {
    Write-Host "[windows-integration] stop pending restart scenario"
    $work = Join-Path $ArtifactsDir "stop-pending-restart"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $launchFile = Join-Path $work "launches.txt"
    $escapedLaunchFile = $launchFile.Replace("'", "''")
    $command = "Add-Content -LiteralPath '$escapedLaunchFile' -Value launch; Start-Sleep -Milliseconds 300; exit 1"
    $config = Join-Path $work "pm_tiny.yaml"
    $yaml = @"
- name: restart_gap
  executable: powershell.exe
  args: ["-NoProfile", "-Command", $(Yaml-Quote $command)]
  cwd: $(Yaml-Quote $work)
  daemon: true
  restart_delay_ms: 2000
  restart_max_delay_ms: 2000
  restart_window_ms: 10000
  restart_max_attempts: 10
  restart_reset_after_ms: 10000
"@
    Write-TestConfig $config $yaml
    Start-TestDaemon "stop-pending-restart" $config
    Wait-For {
        $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        $entry = @($status.processes | Where-Object { $_.name -eq "restart_gap" })
        return $entry.Count -eq 1 -and $entry[0].restart_pending -and $null -eq $entry[0].pid
    } 10 "restart gap pending state"

    Assert-Empty (Invoke-Pm @("stop", "restart_gap", "--no-list")) "stop pending restart output"
    $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
    $entry = @($status.processes | Where-Object { $_.name -eq "restart_gap" })[0]
    if ($entry.state -ne "stopped" -or $entry.restart_pending -or $entry.restart_attempts_in_window -ne 0) {
        throw "stop did not clear pending restart state"
    }
    $launchCount = @(Get-Content $launchFile).Count
    Start-Sleep -Milliseconds 2300
    if (@(Get-Content $launchFile).Count -ne $launchCount) {
        throw "program restarted after stop canceled its pending restart"
    }
    Assert-Empty (Invoke-Pm @("stop", "restart_gap", "--no-list")) "idempotent stop output"

    Assert-Contains (Invoke-Pm @("start", "restart_gap")) 'started `restart_gap` pid=' "manual recovery"
    Wait-For { @(Get-Content $launchFile).Count -gt $launchCount } 5 "manual restart after stop"
    Assert-Empty (Invoke-Pm @("stop", "restart_gap", "--no-list")) "final restart gap stop output"
    Stop-TestDaemon
}

function Run-SaveRecoveryScenario {
    Write-Host "[windows-integration] save recovery scenario"
    $work = Join-Path $ArtifactsDir "save-recovery"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    $yaml = @"
- name: recovery_app
  executable: cmd.exe
  args: ["/c", "ping", "127.0.0.1", "-t"]
  cwd: $(Yaml-Quote $work)
  daemon: false
"@
    Write-TestConfig $config $yaml
    $previousFailureStep = $env:PM_TINY_TEST_FAIL_SAVE_STEP
    try {
        $env:PM_TINY_TEST_FAIL_SAVE_STEP = $null
        Start-TestDaemon "save-recovery-baseline" $config
        Assert-Empty (Invoke-Pm @("save")) "baseline transactional save"
        Stop-TestDaemon
        foreach ($step in @("prepared", "old_moved", "new_installed")) {
            $env:PM_TINY_TEST_FAIL_SAVE_STEP = $step
            Start-TestDaemon "save-recovery-$step" $config
            Assert-Contains (Invoke-PmExpectedFailure @("save")) "save failed" `
                "injected save failure $step"
            Stop-TestDaemon
            $env:PM_TINY_TEST_FAIL_SAVE_STEP = $null
            Start-TestDaemon "save-recovered-$step" $config
            Assert-Contains (Invoke-Pm @("list")) "recovery_app" "save recovery after $step"
            if (Test-Path "$($work)\prog.yaml.save-journal") {
                throw "save recovery left journal after $step"
            }
            Stop-TestDaemon
        }
    } finally {
        $env:PM_TINY_TEST_FAIL_SAVE_STEP = $previousFailureStep
    }
}

function Run-HighProcessCountScenario {
    Write-Host "[windows-integration] high process count scenario"
    $work = Join-Path $ArtifactsDir "high-process-count"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    Start-TestDaemon "high-process-count" $config
    $script:DaemonProcess.Refresh()
    $baselineThreads = $script:DaemonProcess.Threads.Count
    & $ProtocolProbe "connection-churn" "200"
    if ($LASTEXITCODE -ne 0) { throw "short connection warm-up failed" }
    Start-Sleep -Milliseconds 250
    $script:DaemonProcess.Refresh()
    $baselineHandles = $script:DaemonProcess.HandleCount
    $baselinePrivateBytes = $script:DaemonProcess.PrivateMemorySize64
    & $ProtocolProbe "connection-churn" "1000"
    if ($LASTEXITCODE -ne 0) { throw "short connection churn failed" }
    Start-Sleep -Milliseconds 500
    $script:DaemonProcess.Refresh()
    if ($script:DaemonProcess.HandleCount -gt $baselineHandles + 8) {
        throw "daemon handles grew with short connections: baseline=$baselineHandles current=$($script:DaemonProcess.HandleCount)"
    }
    $privateGrowthLimit = 3 * 1024 * 1024
    $privateGrowth = $script:DaemonProcess.PrivateMemorySize64 - $baselinePrivateBytes
    if ($privateGrowth -gt $privateGrowthLimit) {
        throw "daemon private bytes grew with short connections: baseline=$baselinePrivateBytes current=$($script:DaemonProcess.PrivateMemorySize64) delta=$privateGrowth"
    }
    for ($index = 0; $index -lt 100; $index++) {
        $name = "load_$index"
        $output = Invoke-Pm @("start", $name, "--no-daemon", "--", "ping.exe", "-n", "60", "127.0.0.1")
        Assert-Contains $output "started ``$name`` pid=" "high process start $name"
    }
    Wait-For {
        $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        @($status.processes | Where-Object { $_.state -eq "online" }).Count -eq 100
    } 20 "100 managed processes online"
    $script:DaemonProcess.Refresh()
    $loadedThreads = $script:DaemonProcess.Threads.Count
    if ($loadedThreads -gt $baselineThreads + 2) {
        throw "daemon threads grew with process count: baseline=$baselineThreads loaded=$loadedThreads"
    }
    $shutdownStarted = [DateTime]::UtcNow
    Stop-TestDaemon $true 30000
    $shutdownElapsed = ([DateTime]::UtcNow - $shutdownStarted).TotalSeconds
    if ($shutdownElapsed -gt 20) {
        throw "100-process shutdown exceeded 20 seconds: $shutdownElapsed"
    }
}

function Run-LogClientIsolationScenario {
    Write-Host "[windows-integration] log client isolation scenario"
    $work = Join-Path $ArtifactsDir "log-client-isolation"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    Start-TestDaemon "log-client-isolation" $config
    $command = "while (`$true) { [Console]::Out.Write(('x' * 16384)) }"
    Assert-Contains (Invoke-Pm @("start", "flood", "--no-daemon", "--", "powershell.exe",
        "-NoProfile", "-Command", $command)) 'started `flood` pid=' "log flood start"
    & $ProtocolProbe "slow-log" "flood"
    if ($LASTEXITCODE -ne 0) { throw "slow log client was not isolated" }
    Assert-Contains (Invoke-Pm @("version")) "pm_tiny: $script:ExpectedVersion" "daemon after slow log client"
    Wait-For-State "flood" "online" 5 "flood remains online after slow client disconnect"

    & $ProtocolProbe "interrupt-cli" $Cli "flood"
    if ($LASTEXITCODE -ne 0) { throw "pm log Ctrl+C did not return 130" }
    Assert-Contains (Invoke-Pm @("version")) "pm_tiny: $script:ExpectedVersion" "daemon after log Ctrl+C"
    Wait-For-State "flood" "online" 5 "flood remains online after log Ctrl+C"
    Assert-Contains (Invoke-Pm @("stop", "flood")) "flood" "stop log flood list"
    Wait-For-State "flood" "stopped" 10 "log flood stopped state"
    Stop-TestDaemon
}

function Run-LogRotationScenario {
    Write-Host "[windows-integration] log rotation scenario"
    $work = Join-Path $ArtifactsDir "log-rotation"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $config = Join-Path $work "pm_tiny.yaml"
    Write-TestConfig $config "[]`n"
    Start-TestDaemon "log-rotation" $config
    $command = @'
for ($index = 0; $index -lt 4; $index++) {
    $stdoutBlock = [Text.Encoding]::ASCII.GetBytes(([char](65 + $index)).ToString() * 1024)
    $stderrBlock = [Text.Encoding]::ASCII.GetBytes(([char](97 + $index)).ToString() * 1024)
    [Console]::OpenStandardOutput().Write($stdoutBlock, 0, $stdoutBlock.Length)
    [Console]::OpenStandardError().Write($stderrBlock, 0, $stderrBlock.Length)
    Start-Sleep -Milliseconds 20
}
'@
    Assert-Contains (Invoke-Pm @(
        "start", "rotation", "--no-daemon", "--no-pty", "--log-mode", "split",
        "--log-max-size-kb", "1", "--log-archive-count", "2", "--",
        "powershell.exe", "-NoProfile", "-Command", $command
    )) 'started `rotation` pid=' "rotation fixture start"
    Wait-For {
        $status = Invoke-Pm @("list", "--json") | ConvertFrom-Json
        $item = @($status.processes | Where-Object { $_.name -eq "rotation" }) | Select-Object -First 1
        $null -ne $item -and $null -eq $item.pid -and $item.last_exit_code -eq 0
    } 10 "rotation fixture exit"

    foreach ($entry in @(
        @{ stream = "stdout"; expected = @(68, 67, 66) },
        @{ stream = "stderr"; expected = @(100, 99, 98) }
    )) {
        $base = Join-Path $work "logs\rotation_$($entry.stream).log"
        $paths = @($base, "$base.1", "$base.2")
        for ($index = 0; $index -lt $paths.Count; $index++) {
            $path = $paths[$index]
            if (-not (Test-Path -LiteralPath $path)) { throw "rotation archive is missing: $path" }
            if ((Get-Item -LiteralPath $path).Length -ne 1024) {
                throw "rotation archive has unexpected size: $path"
            }
            $first = [IO.File]::ReadAllBytes($path)[0]
            if ($first -ne $entry.expected[$index]) {
                throw "rotation archive ordering mismatch: $path first=$first expected=$($entry.expected[$index])"
            }
        }
        if (Test-Path -LiteralPath "$base.3") { throw "rotation retained too many archives: $base.3" }
    }
    Stop-TestDaemon
}

function Run-CoreScenarios {
    Run-InvalidSddlScenario
    Run-DefaultSddlScenario
    Run-ConnectionFailureScenario
    Run-MainProtocolScenario
    Run-EmptyProgramConfigScenario
    Run-DynamicStartScenario
    Run-DependencyScenario
    Run-WaitingStartLogScenario
    Run-LogHistoryAndRestartGenerationScenario
    Run-SdkScenario
    Run-StartTimeoutScenario
    Run-UnsupportedScenario
    Run-ProcessTreeCase "graceful" 3
    Run-ProcessTreeCase "resistant" 1
    Run-ProcessTreeCase "root-first" 1
    Run-RapidRestartScenario
    Run-CrashLoopScenario
    Run-StopPendingRestartScenario
    Run-SaveRecoveryScenario
    Run-HighProcessCountScenario
    Run-LogClientIsolationScenario
    Run-LogRotationScenario
}

try {
    Remove-Item -Recurse -Force $ArtifactsDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $ArtifactsDir | Out-Null
    if ($Scenario -eq "log-client-isolation") {
        Run-LogClientIsolationScenario
    } elseif ($Scenario -eq "high-process-count") {
        Run-HighProcessCountScenario
    } elseif ($Scenario -eq "lifecycle-transitions") {
        Run-LifecycleTransitionScenario
    } elseif ($Scenario -eq "dependency-mutations" -or $Scenario -eq "dynamic-dependency-delete") {
        Run-DynamicDependencyDeleteScenario
    } elseif ($Scenario -eq "concurrent-controls") {
        Run-ConcurrentControlScenario
    } elseif ($Scenario -eq "core") {
        Run-CoreScenarios
    } elseif ($Scenario -eq "all") {
        Run-CoreScenarios
        Run-LifecycleTransitionScenario
        Run-DynamicDependencyDeleteScenario
        Run-ConcurrentControlScenario
    } else {
        throw "unknown scenario filter: $Scenario"
    }
    Write-Host "Windows protocol integration: PASS"
} catch {
    Write-Error $_
    exit 1
} finally {
    try { Stop-TestDaemon $false } catch { }
    $env:PM_TINY_PIPE_NAME = $script:PreviousPipeName
    $env:PM_TINY_PIPE_SDDL = $script:PreviousPipeSddl
    $env:PM_TINY_HOME = $script:PreviousHome
    $env:PM_TINY_PROG_CFG_FILE = $script:PreviousProgramConfig
    $env:PM_TINY_APP_LOG_DIR = $script:PreviousAppLogDir
    $env:PM_TINY_APP_ENVIRON_DIR = $script:PreviousAppEnvironDir
    $env:PM_TINY_LOG_FILE = $script:PreviousLogFile
}
