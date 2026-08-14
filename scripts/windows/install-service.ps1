#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BinaryPath,
    [Parameter(Mandatory = $true)][string]$ConfigPath,
    [string]$Name = "pm_tiny",
    [string]$DisplayName = "pm_tiny Process Manager",
    [ValidateSet("Automatic", "Manual", "Disabled")][string]$StartupType = "Automatic",
    [string]$HomePath = (Join-Path $env:ProgramData "pm_tiny"),
    [string]$PipeName = "\\.\pipe\pm_tiny",
    [string]$PipeSddl = "D:P(A;;GA;;;SY)(A;;GA;;;BA)",
    [switch]$Start
)

$ErrorActionPreference = "Stop"
$binary = (Resolve-Path $BinaryPath).Path
$config = (Resolve-Path $ConfigPath).Path
$resolvedHome = [System.IO.Path]::GetFullPath($HomePath)
New-Item -ItemType Directory -Force -Path $resolvedHome | Out-Null
$binaryDirectory = Split-Path -Parent $binary
if (Get-Service -Name $Name -ErrorAction SilentlyContinue) {
    throw "Service '$Name' already exists. Uninstall it before reinstalling."
}

$arguments = @(
    '--service',
    '--service-name', ('"' + $Name + '"'),
    '--config', ('"' + $config + '"')
) -join ' '
$commandLine = '"' + $binary + '" ' + $arguments

New-Service -Name $Name -BinaryPathName $commandLine -DisplayName $DisplayName `
    -Description "Supervises configured processes with pm_tiny." -StartupType $StartupType | Out-Null
$serviceRegistryPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name"
$servicePath = "$binaryDirectory;$env:SystemRoot\System32;$env:SystemRoot"
New-ItemProperty -Path $serviceRegistryPath -Name Environment -PropertyType MultiString `
    -Value @(
        "PATH=$servicePath",
        "PM_TINY_HOME=$resolvedHome",
        "PM_TINY_PIPE_NAME=$PipeName",
        "PM_TINY_PIPE_SDDL=$PipeSddl"
    ) -Force | Out-Null
& sc.exe failure $Name reset= 86400 actions= restart/5000/restart/15000/restart/60000 | Out-Null
& sc.exe failureflag $Name 1 | Out-Null

if ($Start) {
    Start-Service -Name $Name
    (Get-Service -Name $Name).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
}

Get-Service -Name $Name
