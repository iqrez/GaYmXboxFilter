[CmdletBinding()]
param(
    [string]$HardwareId = 'USB\VID_045E&PID_02FF&IG_00',
    [string]$InstanceId,
    [string]$DriverInf
)

if (-not $DriverInf) {
    $scriptRoot = Split-Path -Parent $PSCommandPath
    $DriverInf = Join-Path $scriptRoot 'build\driver\GaYmFilter.inf'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run attach_filter.ps1 from an elevated PowerShell prompt.'
}

if (-not (Test-Path $DriverInf)) {
    throw "Driver INF not found: $DriverInf"
}

$pnputil = "$env:SystemRoot\System32\pnputil.exe"
$installOutput = & $pnputil /add-driver $DriverInf /install 2>&1
if ($LASTEXITCODE -ne 0) {
    throw ("pnputil failed to add/install driver package.`n" + ($installOutput -join [Environment]::NewLine))
}

if (-not $InstanceId) {
    $deviceOutput = & $pnputil /enum-devices /connected /class HIDClass 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "pnputil failed while enumerating connected HIDClass devices."
    }

    $instanceMatch = $deviceOutput |
        Select-String '^\s*Instance ID:\s*(.+)$' |
        Where-Object {
            $_.Matches[0].Groups[1].Value.Trim().StartsWith($HardwareId, [StringComparison]::OrdinalIgnoreCase)
        } |
        Select-Object -First 1

    if (-not $instanceMatch) {
        throw "No connected device matches hardware ID $HardwareId."
    }

    $InstanceId = $instanceMatch.Matches[0].Groups[1].Value.Trim()
}

$restartOutput = & $pnputil /restart-device $InstanceId 2>&1
$restartExitCode = $LASTEXITCODE

$stackOutput = & $pnputil /enum-devices /instanceid $InstanceId /services /stack /drivers 2>&1
if ($LASTEXITCODE -ne 0) {
    throw ("pnputil failed while querying the live stack for $InstanceId.`n" + ($stackOutput -join [Environment]::NewLine))
}

$stackText = $stackOutput -join [Environment]::NewLine
$hasGaYmFilter = $stackText -match '(?im)^\s*GaYmFilter\s*$'
$hasHidUsb = $stackText -match '(?im)^\s*(?:Stack:\s*)?HidUsb\s*$'

Write-Host ($installOutput -join [Environment]::NewLine)
Write-Host ''
Write-Host "Device Instance: $InstanceId"
Write-Host ''
Write-Host 'Restart output:'
Write-Host ($restartOutput -join [Environment]::NewLine)
Write-Host ''
Write-Host 'Live stack:'
Write-Host $stackText
Write-Host ''

if (-not $hasGaYmFilter) {
    if (($restartOutput -join [Environment]::NewLine) -match 'pending system reboot') {
        throw "Windows reports that $InstanceId is pending a system reboot. Reboot once, reconnect the controller, and rerun attach_filter.ps1 to verify the live stack."
    }

    if ($restartExitCode -ne 0) {
        throw "pnputil failed to restart $InstanceId before stack verification."
    }

    throw "GaYmFilter is not present in the live stack for $InstanceId."
}

if (-not $hasHidUsb) {
    throw "HidUsb is not present in the live stack for $InstanceId. The filter is not attached on the USB HID path."
}

if ($restartExitCode -ne 0) {
    Write-Warning 'pnputil reported that additional reboot or device reconfiguration may still be pending, but the verified live stack already includes GaYmFilter.'
}

Write-Host 'GaYmFilter is present on the USB HID stack alongside HidUsb.'
