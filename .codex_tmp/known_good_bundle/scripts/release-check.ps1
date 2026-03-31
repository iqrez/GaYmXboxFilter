[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Build,
    [switch]$KeepInstalled,
    [switch]$SkipTransitionChecks,
    [switch]$SkipRollbackCycle,
    [switch]$ResumeAfterReboot,
    [string]$StateFile
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common-paths.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $Configuration
$defaultStateFile = New-GaYmStatePath -Layout $layout -LeafName 'release-check-state.json'
$installStatePath = New-GaYmStatePath -Layout $layout -LeafName 'install-driver-state.json'
if (-not $StateFile) {
    $StateFile = $defaultStateFile
}

function Read-ReleaseCheckState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        return $null
    }

    return Get-Content -Path $Path -Raw | ConvertFrom-Json
}

function Write-ReleaseCheckState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Configuration,
        [Parameter(Mandatory = $true)]
        [string]$Phase,
        [Parameter(Mandatory = $true)]
        [bool]$KeepInstalled,
        [Parameter(Mandatory = $true)]
        [bool]$SkipTransitionChecks,
        [Parameter(Mandatory = $true)]
        [bool]$SkipRollbackCycle
    )

    $stateDirectory = Split-Path -Parent $Path
    if (-not (Test-Path $stateDirectory)) {
        New-Item -ItemType Directory -Path $stateDirectory -Force | Out-Null
    }

    $state = [ordered]@{
        Version              = 1
        Phase                = $Phase
        Configuration        = $Configuration
        KeepInstalled        = $KeepInstalled
        SkipTransitionChecks = $SkipTransitionChecks
        SkipRollbackCycle    = $SkipRollbackCycle
        SavedAt              = (Get-Date).ToString('o')
    }

    $state | ConvertTo-Json | Set-Content -Path $Path -Encoding ASCII
}

function Read-InstallState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        return $null
    }

    return Get-Content -Path $Path -Raw | ConvertFrom-Json
}

function Get-CurrentBootTimeUtc {
    return (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime.ToUniversalTime()
}

function Remove-ReleaseCheckState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (Test-Path $Path) {
        Remove-Item -Path $Path -Force
    }
}

$pendingState = Read-ReleaseCheckState -Path $StateFile
if ($pendingState -and -not $ResumeAfterReboot) {
    Write-Host ("Detected pending release-check resume state at {0}. Resuming post-reboot validation." -f $StateFile)
    $ResumeAfterReboot = $true
}

$effectiveConfiguration = $Configuration
$effectiveKeepInstalled = [bool]$KeepInstalled
$effectiveSkipTransitionChecks = [bool]$SkipTransitionChecks
$effectiveSkipRollbackCycle = [bool]$SkipRollbackCycle

if ($ResumeAfterReboot) {
    if (-not $pendingState) {
        throw "No pending release-check state was found at $StateFile."
    }

    if ($PSBoundParameters.ContainsKey('Configuration') -and $Configuration -ne $pendingState.Configuration) {
        throw ("Resume state expects configuration '{0}', but '{1}' was requested." -f $pendingState.Configuration, $Configuration)
    }

    $effectiveConfiguration = [string]$pendingState.Configuration
    $effectiveKeepInstalled = [bool]$pendingState.KeepInstalled
    $effectiveSkipTransitionChecks = [bool]$pendingState.SkipTransitionChecks
    $effectiveSkipRollbackCycle = [bool]$pendingState.SkipRollbackCycle
}

$layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $effectiveConfiguration
$securityVerifier = Get-GaYmToolPath -Layout $layout -Name 'SecurityAutoVerify.exe'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run release-check.ps1 from an elevated PowerShell session.'
    }
}

Assert-Administrator

function Invoke-RequiredScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action
    )

    Write-Host ''
    Write-Host "=== $Name ==="
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed."
    }
}

function Invoke-ValidationPass {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    Invoke-RequiredScript -Name "$Label Smoke" -Action {
        & (Join-Path $PSScriptRoot 'smoke-test.ps1') -Configuration $effectiveConfiguration -IncludeRuntimeVerifiers
    }

    if (-not (Test-Path $securityVerifier)) {
        throw "SecurityAutoVerify.exe not found at $securityVerifier."
    }

    Invoke-RequiredScript -Name "$Label Security" -Action {
        & $securityVerifier
    }

    if (-not $effectiveSkipTransitionChecks) {
        Invoke-RequiredScript -Name "$Label Transitions" -Action {
            & (Join-Path $PSScriptRoot 'transition-check.ps1') -Configuration $effectiveConfiguration
        }
    }
}

function Get-HidChildInstanceId {
    $devices = Get-PnpDevice -Class HIDClass -ErrorAction Stop
    $match = $devices |
        Where-Object { $_.InstanceId -like 'HID\VID_045E&PID_02FF&IG_00*' -and ($_.Present -eq $true -or $_.Status -eq 'OK') } |
        Select-Object -First 1
    if (-not $match) {
        throw 'The supported HID child `HID\VID_045E&PID_02FF&IG_00` is not present.'
    }

    return $match.InstanceId
}

function Get-LiveStackText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstanceId
    )

    $pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
    $stackOutput = & $pnputil /enum-devices /instanceid $InstanceId /stack /drivers 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw ("pnputil failed while querying the live stack for $InstanceId.`n" + ($stackOutput -join [Environment]::NewLine))
    }

    return ($stackOutput -join [Environment]::NewLine)
}

function Get-StackDriverNames {
    param(
        [Parameter(Mandatory = $true)]
        [string]$StackText
    )

    $stackNames = New-Object 'System.Collections.Generic.List[string]'
    $capturing = $false

    foreach ($line in ($StackText -split "\r?\n")) {
        if (-not $capturing) {
            if ($line -match '^\s*Stack:\s*(.+?)\s*$') {
                $stackNames.Add($matches[1].Trim())
                $capturing = $true
            }

            continue
        }

        if ($line -match '^\s*Matching Drivers:\s*$' -or $line -match '^\s*$') {
            break
        }

        if ($line -match '^\s+(.+?)\s*$') {
            $stackNames.Add($matches[1].Trim())
            continue
        }

        break
    }

    return $stackNames.ToArray()
}

function Test-SupportedHybridStack {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$StackNames
    )

    $expectedStack = @('GaYmXInputFilter', 'xinputhid', 'GaYmFilter', 'HidUsb')
    if ($StackNames.Count -ne $expectedStack.Count) {
        return $false
    }

    for ($index = 0; $index -lt $expectedStack.Count; $index++) {
        if ($StackNames[$index] -ine $expectedStack[$index]) {
            return $false
        }
    }

    return $true
}

function Test-DeviceRebootRequired {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstanceId
    )

    try {
        $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_IsRebootRequired' -ErrorAction Stop
        return ([bool]$property.Data)
    } catch {
        return $false
    }
}

function Test-RebootBoundaryException {
    param(
        [Parameter(Mandatory = $true)]
        [System.Exception]$Exception
    )

    return ($Exception.Message -like '*reboot-required state*')
}

function Pause-ReleaseCheckForReboot {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('AwaitingPass1Validation', 'AwaitingPass2Validation')]
        [string]$Phase
    )

    Write-ReleaseCheckState -Path $StateFile `
        -Configuration $effectiveConfiguration `
        -Phase $Phase `
        -KeepInstalled $effectiveKeepInstalled `
        -SkipTransitionChecks $effectiveSkipTransitionChecks `
        -SkipRollbackCycle $effectiveSkipRollbackCycle

    $script:pausedForReboot = $true

    $resumeLabel = if ($Phase -eq 'AwaitingPass1Validation') { 'pass-1 validation' } else { 'pass-2 validation' }
    Write-Warning ("Release check paused at the reboot boundary. Reboot, then rerun release-check.ps1 to resume {0}. Resume state: {1}" -f $resumeLabel, $StateFile)
}

function Test-InstallRebootBoundaryCleared {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('AwaitingPass1Validation', 'AwaitingPass2Validation')]
        [string]$Phase
    )

    $pendingInstallState = Read-InstallState -Path $installStatePath
    if (-not $pendingInstallState) {
        return $true
    }

    $savedBootTimeUtc = [datetime]::Parse([string]$pendingInstallState.BootTimeUtc).ToUniversalTime()
    $currentBootTimeUtc = Get-CurrentBootTimeUtc
    if ($currentBootTimeUtc -gt $savedBootTimeUtc) {
        return $true
    }

    $pendingInstanceId = [string]$pendingInstallState.InstanceId
    if (-not $pendingInstanceId) {
        $pendingInstanceId = Get-HidChildInstanceId
    }

    try {
        $stackText = Get-LiveStackText -InstanceId $pendingInstanceId
        $stackNames = Get-StackDriverNames -StackText $stackText
        $deviceRebootRequired = Test-DeviceRebootRequired -InstanceId $pendingInstanceId
        if ((Test-SupportedHybridStack -StackNames $stackNames) -and -not $deviceRebootRequired) {
            Remove-Item -Path $installStatePath -Force
            return $true
        }
    } catch {
        # Keep the checkpoint until a reboot or a clearly healthy live stack is observed.
    }

    Pause-ReleaseCheckForReboot -Phase $Phase
    Write-Warning ("install-driver.ps1 already reported a reboot-required boundary on this boot. Reboot before running release-check.ps1. Pending install state: {0}" -f $installStatePath)
    return $false
}

function Resume-ReleaseCheck {
    switch ([string]$pendingState.Phase) {
        'AwaitingPass1Validation' {
            Invoke-ValidationPass -Label 'Pass 1'

            if (-not $effectiveSkipRollbackCycle) {
                Invoke-RequiredScript -Name 'Rollback Uninstall' -Action {
                    & (Join-Path $PSScriptRoot 'uninstall-driver.ps1')
                }

                try {
                    Invoke-RequiredScript -Name 'Install Pass 2' -Action {
                        & (Join-Path $PSScriptRoot 'install-driver.ps1') -Configuration $effectiveConfiguration -FailOnRebootRequired
                    }
                } catch {
                    if (Test-RebootBoundaryException -Exception $_.Exception) {
                        Pause-ReleaseCheckForReboot -Phase 'AwaitingPass2Validation'
                        return
                    }

                    throw
                }

                Invoke-ValidationPass -Label 'Pass 2'
            }
        }
        'AwaitingPass2Validation' {
            Invoke-ValidationPass -Label 'Pass 2'
        }
        default {
            throw ("Unsupported release-check resume phase '{0}' in {1}." -f $pendingState.Phase, $StateFile)
        }
    }
}

$pausedForReboot = $false

try {
    if ($ResumeAfterReboot) {
        if (-not (Test-InstallRebootBoundaryCleared -Phase ([string]$pendingState.Phase))) {
            return
        }

        Resume-ReleaseCheck
        if ($pausedForReboot) {
            return
        }

        Remove-ReleaseCheckState -Path $StateFile

        Write-Host ''
        Write-Host 'Release check PASS.'
        return
    }

    if ($Build) {
        $buildDriverScript = Join-Path $PSScriptRoot 'build-driver.ps1'
        $buildToolsScript = Join-Path $PSScriptRoot 'build-tools.ps1'
        if (-not (Test-Path $buildDriverScript) -or -not (Test-Path $buildToolsScript)) {
            throw 'This extracted bundle does not include build scripts. Rebuild from the full repo or run release-check against the bundled artifacts directly.'
        }

        & $buildDriverScript -Configuration $effectiveConfiguration
        & $buildToolsScript -Configuration $effectiveConfiguration

        $layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $effectiveConfiguration
        $securityVerifier = Get-GaYmToolPath -Layout $layout -Name 'SecurityAutoVerify.exe'
    }

    if (-not (Test-InstallRebootBoundaryCleared -Phase 'AwaitingPass1Validation')) {
        return
    }

    try {
        Invoke-RequiredScript -Name 'Install Pass 1' -Action {
            & (Join-Path $PSScriptRoot 'install-driver.ps1') -Configuration $effectiveConfiguration -FailOnRebootRequired
        }
    } catch {
        if (Test-RebootBoundaryException -Exception $_.Exception) {
            Pause-ReleaseCheckForReboot -Phase 'AwaitingPass1Validation'
            return
        }

        throw
    }

    Invoke-ValidationPass -Label 'Pass 1'

    if (-not $effectiveSkipRollbackCycle) {
        Invoke-RequiredScript -Name 'Rollback Uninstall' -Action {
            & (Join-Path $PSScriptRoot 'uninstall-driver.ps1')
        }
        try {
            Invoke-RequiredScript -Name 'Install Pass 2' -Action {
                & (Join-Path $PSScriptRoot 'install-driver.ps1') -Configuration $effectiveConfiguration -FailOnRebootRequired
            }
        } catch {
            if (Test-RebootBoundaryException -Exception $_.Exception) {
                Pause-ReleaseCheckForReboot -Phase 'AwaitingPass2Validation'
                return
            }

            throw
        }

        Invoke-ValidationPass -Label 'Pass 2'
    }
    Remove-ReleaseCheckState -Path $StateFile

    Write-Host ''
    Write-Host 'Release check PASS.'
}
finally {
    if (-not $pausedForReboot -and -not $effectiveKeepInstalled) {
        Write-Host ''
        Write-Host '=== Final Rollback ==='
        try {
            & (Join-Path $PSScriptRoot 'uninstall-driver.ps1')
        } catch {
            Write-Warning "Rollback failed: $($_.Exception.Message)"
        }
    }
}
