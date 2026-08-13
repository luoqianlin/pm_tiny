#Requires -RunAsAdministrator
[CmdletBinding()]
param([string]$Name = "pm_tiny")

$ErrorActionPreference = "Stop"
$service = Get-Service -Name $Name -ErrorAction SilentlyContinue
if (-not $service) {
    Write-Host "Service '$Name' is not installed."
    exit 0
}
if ($service.Status -ne 'Stopped') {
    Stop-Service -Name $Name
    $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
}
& sc.exe delete $Name | Out-Null
for ($attempt = 0; $attempt -lt 50; $attempt++) {
    if (-not (Get-Service -Name $Name -ErrorAction SilentlyContinue)) { exit 0 }
    Start-Sleep -Milliseconds 100
}
throw "Service '$Name' was not deleted within 5 seconds."
