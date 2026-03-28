[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$BuildTools,
    [switch]$RestartParent,
    [switch]$InteractiveUnplugReplug,
    [switch]$InteractiveSleepResume,
    [int]$TransitionTimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$stageRoot = if ($Configuration -eq 'Debug') { Join-Path $repoRoot 'out\dev' } else { Join-Path $repoRoot 'out\release' }
$toolsRoot = Join-Path $stageRoot 'tools'
$cli = Join-Path $toolsRoot 'GaYmCLI.exe'
$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run transition-check.ps1 from an elevated PowerShell session.'
    }
}

function Get-HidChildInstanceId {
    $device = Get-PnpDevice -Class HIDClass -ErrorAction Stop |
        Where-Object { $_.InstanceId -like 'HID\VID_045E&PID_02FF&IG_00*' -and ($_.Present -eq $true -or $_.Status -eq 'OK') } |
        Select-Object -First 1

    if ($device) {
        return $device.InstanceId
    }

    return $null
}

function Get-CompositeParentInstanceId {
    $device = Get-PnpDevice -ErrorAction Stop |
        Where-Object { $_.InstanceId -like 'USB\VID_045E&PID_0B12*' -and ($_.Present -eq $true -or $_.Status -eq 'OK') } |
        Select-Object -First 1

    if ($device) {
        return $device.InstanceId
    }

    return $null
}

function Wait-ForDevice {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Probe,
        [Parameter(Mandatory = $true)]
        [string]$Description,
        [int]$TimeoutSeconds = 30
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $value = & $Probe
        if ($value) {
            return $value
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)

    throw "$Description did not return within $TimeoutSeconds seconds."
}

function Wait-ForDeviceAbsence {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Probe,
        [Parameter(Mandatory = $true)]
        [string]$Description,
        [int]$TimeoutSeconds = 30
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $value = & $Probe
        if (-not $value) {
            return
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)

    throw "$Description did not disappear within $TimeoutSeconds seconds."
}

function Invoke-UpperCli {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $previousTarget = $env:GAYM_CONTROL_TARGET
    $env:GAYM_CONTROL_TARGET = 'upper'
    try {
        & $cli @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "GaYmCLI $($Arguments -join ' ') failed."
        }
    } finally {
        if ($null -eq $previousTarget) {
            Remove-Item Env:GAYM_CONTROL_TARGET -ErrorAction SilentlyContinue
        } else {
            $env:GAYM_CONTROL_TARGET = $previousTarget
        }
    }
}

function Assert-OverrideCleared {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $status = & $cli status 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "GaYmCLI status failed after $Label."
    }

    $statusText = $status -join [Environment]::NewLine
    Write-Host ''
    Write-Host "=== Post-$Label status ==="
    Write-Host $statusText
    if ($statusText -notmatch 'Override:OFF') {
        throw "Override did not clear after $Label."
    }
}

function Invoke-DeviceRestart {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstanceId,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $output = & $pnputil /restart-device $InstanceId 2>&1
    $exitCode = $LASTEXITCODE
    $outputText = $output -join [Environment]::NewLine

    if ($outputText) {
        Write-Host $outputText
    }

    if ($exitCode -eq 0) {
        return @{
            Restarted = $true
            Skipped = $false
            Reason = $null
        }
    }

    if ($outputText -match 'System reboot is needed to complete configuration operations!' -or
        $outputText -match 'This command is not supported on this OS product\.') {
        Write-Warning "$Description restart skipped: Windows blocked in-session restart for this device."
        return @{
            Restarted = $false
            Skipped = $true
            Reason = $outputText
        }
    }

    throw "pnputil /restart-device failed for $Description."
}

Assert-Administrator

if ($BuildTools) {
    & (Join-Path $PSScriptRoot 'build-tools.ps1') -Configuration $Configuration
}

if (-not (Test-Path $cli)) {
    throw "GaYmCLI.exe not found at $cli. Run scripts\build-tools.ps1 first or use -BuildTools."
}

$hidChild = Get-HidChildInstanceId
if (-not $hidChild) {
    throw 'The supported HID child is not present.'
}

Write-Host '=== Prime override state ==='
Invoke-UpperCli -Arguments @('on', '0')
Invoke-UpperCli -Arguments @('report', '0', '1', '0', '15', '0', '0', '32767', '0', '0', '0')
Start-Sleep -Milliseconds 250

try {
    $skipped = $false

    Write-Host ''
    Write-Host "=== Restart HID child: $hidChild ==="
    $hidRestart = Invoke-DeviceRestart -InstanceId $hidChild -Description 'the HID child'
    if ($hidRestart.Skipped) {
        $skipped = $true
    } else {
        $hidChild = Wait-ForDevice -Probe ${function:Get-HidChildInstanceId} -Description 'HID child re-enumeration' -TimeoutSeconds $TransitionTimeoutSeconds
        Assert-OverrideCleared -Label 'HID child restart'
    }

    if (-not $skipped -and $RestartParent) {
        $parent = Get-CompositeParentInstanceId
        if (-not $parent) {
            throw 'The composite parent is not present.'
        }

        Write-Host ''
        Write-Host "=== Restart composite parent: $parent ==="
        $parentRestart = Invoke-DeviceRestart -InstanceId $parent -Description 'the composite parent'
        if ($parentRestart.Skipped) {
            $skipped = $true
        } else {
            Wait-ForDevice -Probe ${function:Get-CompositeParentInstanceId} -Description 'Composite parent re-enumeration' -TimeoutSeconds $TransitionTimeoutSeconds | Out-Null
            Wait-ForDevice -Probe ${function:Get-HidChildInstanceId} -Description 'HID child re-enumeration after parent restart' -TimeoutSeconds $TransitionTimeoutSeconds | Out-Null
            Assert-OverrideCleared -Label 'parent restart'
        }
    }

    if ($InteractiveUnplugReplug) {
        Write-Host ''
        Read-Host 'Unplug the controller now, then press Enter to begin disappearance monitoring'
        Wait-ForDeviceAbsence -Probe ${function:Get-HidChildInstanceId} -Description 'HID child after unplug' -TimeoutSeconds $TransitionTimeoutSeconds
        Read-Host 'Replug the controller now, then press Enter to begin re-enumeration monitoring'
        Wait-ForDevice -Probe ${function:Get-HidChildInstanceId} -Description 'HID child re-enumeration after unplug/replug' -TimeoutSeconds $TransitionTimeoutSeconds | Out-Null
        Assert-OverrideCleared -Label 'unplug/replug'
    }

    if ($InteractiveSleepResume) {
        Write-Host ''
        Read-Host 'Put the machine to sleep now, wake it, unlock it, then press Enter to continue'
        Wait-ForDevice -Probe ${function:Get-HidChildInstanceId} -Description 'HID child after sleep/resume' -TimeoutSeconds $TransitionTimeoutSeconds | Out-Null
        Assert-OverrideCleared -Label 'sleep/resume'
    }
}
finally {
    try {
        Invoke-UpperCli -Arguments @('off', '0')
    } catch {
        Write-Warning "Best-effort override cleanup failed: $($_.Exception.Message)"
    }
}

Write-Host ''
if ($skipped) {
    Write-Host 'Transition check SKIPPED: Windows blocked in-session device restart.'
} else {
    Write-Host 'Transition check PASS.'
}
