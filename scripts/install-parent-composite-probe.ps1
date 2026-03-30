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
        return Join-Path $repoRoot 'out\dev\polling-composite-probe\package'
    }

    return Join-Path $repoRoot 'out\release\polling-composite-probe\package'
}

function Get-CompositeParentInstance {
    return Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like 'USB\VID_045E&PID_0B12*' } |
        Sort-Object Status, InstanceId |
        Select-Object -First 1
}

function Test-DeviceRebootRequired {
    param([string]$InstanceId)

    try {
        $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_IsRebootRequired'
        return $property.Data -eq $true
    } catch {
        return $false
    }
}

Assert-Administrator

$packageRoot = Get-ProbeStageRoot -ConfigurationName $Configuration
$probeInf = Join-Path $packageRoot 'GaYmCompositeProbe.inf'

if (-not (Test-Path $probeInf)) {
    throw "Composite parent probe INF not found: $probeInf. Run scripts\build-parent-composite-probe-package.ps1 first."
}

$parentDevice = Get-CompositeParentInstance
if (-not $parentDevice) {
    throw 'The composite parent 0B12 device is not currently present. Connect the controller before installing the probe package.'
}

Write-Host "Installing composite parent probe package: $probeInf"
$output = & pnputil /add-driver $probeInf /install
$output | ForEach-Object { Write-Host $_ }

if ($LASTEXITCODE -ne 0 -and
    ($output -join "`n") -notmatch 'System reboot is needed' -and
    ($output -join "`n") -notmatch 'Already exists in the system' -and
    ($output -join "`n") -notmatch 'up-to-date on device') {
    throw 'pnputil failed while installing the composite parent probe package.'
}

Write-Host ''
Write-Host "Composite parent instance: $($parentDevice.InstanceId)"
& pnputil /enum-devices /instanceid $parentDevice.InstanceId /stack /drivers

if (Test-DeviceRebootRequired -InstanceId $parentDevice.InstanceId) {
    Write-Warning 'The composite parent probe package is staged, but the device is still in a reboot-required state. Reboot before evaluating cadence changes.'
}
