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

$repoRoot = Split-Path -Parent $PSScriptRoot
$defaultStateFile = Join-Path $repoRoot 'out\release-check-state.json'
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
        Phase                = 'AwaitingPostRebootValidation'
        Configuration        = $Configuration
        KeepInstalled        = $KeepInstalled
        SkipTransitionChecks = $SkipTransitionChecks
        SkipRollbackCycle    = $SkipRollbackCycle
        SavedAt              = (Get-Date).ToString('o')
    }

    $state | ConvertTo-Json | Set-Content -Path $Path -Encoding ASCII
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

$stageRoot = if ($effectiveConfiguration -eq 'Debug') { Join-Path $repoRoot 'out\dev' } else { Join-Path $repoRoot 'out\release' }
$toolsRoot = Join-Path $stageRoot 'tools'
$securityVerifier = Join-Path $toolsRoot 'SecurityAutoVerify.exe'

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

function Resume-ReleaseCheck {
    Invoke-ValidationPass -Label 'Pass 2'
}

$pausedForReboot = $false

try {
    if ($ResumeAfterReboot) {
        Resume-ReleaseCheck
        Remove-ReleaseCheckState -Path $StateFile

        Write-Host ''
        Write-Host 'Release check PASS.'
        return
    }

    if ($Build) {
        & (Join-Path $PSScriptRoot 'build-driver.ps1') -Configuration $effectiveConfiguration
        & (Join-Path $PSScriptRoot 'build-tools.ps1') -Configuration $effectiveConfiguration
    }

    Invoke-RequiredScript -Name 'Install Pass 1' -Action {
        & (Join-Path $PSScriptRoot 'install-driver.ps1') -Configuration $effectiveConfiguration -FailOnRebootRequired
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
            if ($_.Exception.Message -like '*reboot-required state*') {
                Write-ReleaseCheckState -Path $StateFile `
                    -Configuration $effectiveConfiguration `
                    -KeepInstalled $effectiveKeepInstalled `
                    -SkipTransitionChecks $effectiveSkipTransitionChecks `
                    -SkipRollbackCycle $effectiveSkipRollbackCycle

                $pausedForReboot = $true
                Write-Warning ("Release check paused at the reboot boundary. Reboot, then rerun release-check.ps1 to resume pass-2 validation. Resume state: {0}" -f $StateFile)
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
