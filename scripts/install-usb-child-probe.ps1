[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated PowerShell session.'
    }
}

function Get-ProbeStageRoot {
    param([string]$ConfigurationName)

    $repoRoot = Split-Path -Parent $PSScriptRoot
    if ($ConfigurationName -eq 'Debug') {
        return Join-Path $repoRoot 'out\dev\polling-usb-child-probe\package'
    }

    return Join-Path $repoRoot 'out\release\polling-usb-child-probe\package'
}

function Get-UsbChildInstance {
    return Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like 'USB\VID_045E&PID_02FF&IG_00*' } |
        Sort-Object Status, InstanceId |
        Select-Object -First 1
}

Assert-Administrator

$packageRoot = Get-ProbeStageRoot -ConfigurationName $Configuration
$probeInf = Join-Path $packageRoot 'GaYmUsbChildProbe.inf'

if (-not (Test-Path $probeInf)) {
    throw "USB-child probe INF not found: $probeInf. Run scripts\build-usb-child-probe-package.ps1 first."
}

$usbChild = Get-UsbChildInstance
if (-not $usbChild) {
    throw 'The USB 02FF child is not currently present. Connect the controller before installing the probe package.'
}

Write-Host "Installing USB-child probe package: $probeInf"
$output = & pnputil /add-driver $probeInf /install
$output | ForEach-Object { Write-Host $_ }

if ($LASTEXITCODE -ne 0) {
    throw 'pnputil failed while installing the USB-child probe package.'
}

Write-Host ''
Write-Host "USB child instance: $($usbChild.InstanceId)"
& pnputil /enum-devices /instanceid $usbChild.InstanceId /stack /drivers
