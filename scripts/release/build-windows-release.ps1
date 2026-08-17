[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$SourceDir = (Resolve-Path (Join-Path $PSScriptRoot "..\..")),
    [string]$BuildDir = "",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $SourceDir "build-release-windows-x64-msvc" }
if ([string]::IsNullOrWhiteSpace($OutputDir)) { $OutputDir = Join-Path $BuildDir "release-bin" }

& cmake.exe -S $SourceDir -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
    -DPM_TINY_BUILD_TESTS=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON
if ($LASTEXITCODE -ne 0) { throw "Windows Release configure failed" }
& cmake.exe --build $BuildDir --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw "Windows Release build failed" }
& ctest.exe --test-dir $BuildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Windows Release tests failed" }

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
foreach ($name in @("pm_tiny.exe", "pm.exe")) {
    $source = Join-Path (Join-Path $BuildDir "Release") $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Missing Windows Release binary: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDir $name) -Force
}
if (Test-Path -LiteralPath (Join-Path $OutputDir "pm2.exe")) { throw "Unexpected pm2.exe" }
$version = (Get-Content -LiteralPath (Join-Path $SourceDir "VERSION") -Raw).Trim()
$daemonVersion = (& (Join-Path $OutputDir "pm_tiny.exe") --version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or -not $daemonVersion.Contains("pm_tiny $version")) {
    throw "Windows Release version mismatch: $daemonVersion"
}
$compiler = (& cmd.exe /d /s /c '""%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property catalog_productDisplayVersion"' 2>&1 | Out-String).Trim()
@("toolchain=VS 2022/MSVC", "visual_studio=$compiler") |
    Set-Content -LiteralPath (Join-Path $OutputDir "toolchain.txt") -Encoding ASCII
Write-Output "windows release binaries: $OutputDir"
