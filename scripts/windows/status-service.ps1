[CmdletBinding()]
param([string]$Name = "pm_tiny")

$service = Get-CimInstance Win32_Service -Filter "Name='$($Name.Replace("'", "''"))'" -ErrorAction SilentlyContinue
if (-not $service) {
    Write-Error "Service '$Name' is not installed."
    exit 1
}
$service | Select-Object Name, DisplayName, State, StartMode, ProcessId, StartName, PathName
