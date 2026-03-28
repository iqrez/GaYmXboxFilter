[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$BuildTools,
    [switch]$RestartParent,
    [switch]$InteractiveUnplugReplug,
    [switch]$InteractiveSleepResume,
    [int]$TransitionTimeoutSeconds = 30,
    [string]$StateFile
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common-paths.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $Configuration
$defaultStateFile = New-GaYmStatePath -Layout $layout -LeafName 'transition-check-state.json'
if (-not $StateFile) {
    $StateFile = $defaultStateFile
}

function Read-TransitionState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        return $null
    }

    return Get-Content -Path $Path -Raw | ConvertFrom-Json
}

function Write-TransitionState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Configuration,
        [Parameter(Mandatory = $true)]
        [ValidateSet('UnplugReplug', 'SleepResume')]
        [string]$Mode
    )

    $directory = Split-Path -Parent $Path
    if (-not (Test-Path $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    $state = [ordered]@{
        Version       = 1
        Mode          = $Mode
        Configuration = $Configuration
        SavedAt       = (Get-Date).ToString('o')
    }

    $state | ConvertTo-Json | Set-Content -Path $Path -Encoding ASCII
}

function Remove-TransitionState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (Test-Path $Path) {
        Remove-Item -Path $Path -Force
    }
}

$pendingState = Read-TransitionState -Path $StateFile
$effectiveConfiguration = $Configuration
if ($pendingState) {
    if ($PSBoundParameters.ContainsKey('Configuration') -and $Configuration -ne $pendingState.Configuration) {
        throw ("Pending transition state expects configuration '{0}', but '{1}' was requested." -f $pendingState.Configuration, $Configuration)
    }

    $effectiveConfiguration = [string]$pendingState.Configuration
}

$layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $effectiveConfiguration
$cli = Get-GaYmToolPath -Layout $layout -Name 'GaYmCLI.exe'

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

function Resume-ManualTransition {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('UnplugReplug', 'SleepResume')]
        [string]$Mode
    )

    $label = if ($Mode -eq 'UnplugReplug') { 'manual unplug/replug' } else { 'manual sleep/resume' }
    Wait-ForDevice -Probe ${function:Get-HidChildInstanceId} -Description 'HID child re-enumeration' -TimeoutSeconds $TransitionTimeoutSeconds | Out-Null
    Assert-OverrideCleared -Label $label
    Remove-TransitionState -Path $StateFile

    Write-Host ''
    Write-Host ("Transition check PASS ({0})." -f $label)
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
    $buildToolsScript = Join-Path $PSScriptRoot 'build-tools.ps1'
    if (-not (Test-Path $buildToolsScript)) {
        throw 'This extracted bundle does not include build-tools.ps1. Use the bundled tools directly or rebuild from the full repo.'
    }

    & $buildToolsScript -Configuration $effectiveConfiguration

    $layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $effectiveConfiguration
    $cli = Get-GaYmToolPath -Layout $layout -Name 'GaYmCLI.exe'
}

if (-not (Test-Path $cli)) {
    if ($layout.Mode -eq 'Bundle') {
        throw "GaYmCLI.exe not found inside the extracted bundle: $cli. Re-extract the bundle or regenerate it with scripts\\package-bundle.ps1 from the repo."
    }

    throw "GaYmCLI.exe not found at $cli. Run scripts\build-tools.ps1 first or use -BuildTools."
}

if ($InteractiveUnplugReplug -and $InteractiveSleepResume) {
    throw 'Choose only one manual transition mode at a time.'
}

if ($pendingState) {
    Resume-ManualTransition -Mode ([string]$pendingState.Mode)
    return
}

$hidChild = Get-HidChildInstanceId
if (-not $hidChild) {
    throw 'The supported HID child is not present.'
}

Write-Host '=== Prime override state ==='
Invoke-UpperCli -Arguments @('on', '0')
Invoke-UpperCli -Arguments @('report', '0', '1', '0', '15', '0', '0', '32767', '0', '0', '0')
Start-Sleep -Milliseconds 250

$pausedForManualTransition = $false

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
        Write-TransitionState -Path $StateFile -Configuration $effectiveConfiguration -Mode 'UnplugReplug'
        $pausedForManualTransition = $true
        Write-Host ("Transition state saved to {0}" -f $StateFile)
        Write-Host 'Unplug the controller completely, plug it back in, then rerun this command to resume validation.'
        return
    }

    if ($InteractiveSleepResume) {
        Write-Host ''
        Write-TransitionState -Path $StateFile -Configuration $effectiveConfiguration -Mode 'SleepResume'
        $pausedForManualTransition = $true
        Write-Host ("Transition state saved to {0}" -f $StateFile)
        Write-Host 'Put the machine to sleep, wake it, unlock it, then rerun this command to resume validation.'
        return
    }
}
finally {
    if (-not $pausedForManualTransition) {
        try {
            Invoke-UpperCli -Arguments @('off', '0')
        } catch {
            Write-Warning "Best-effort override cleanup failed: $($_.Exception.Message)"
        }
    }
}

Write-Host ''
if ($skipped) {
    Write-Host 'Transition check SKIPPED: Windows blocked in-session device restart.'
} else {
    Write-Host 'Transition check PASS.'
}
