[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Build,
    [switch]$KeepInstalled,
    [switch]$SkipTransitionChecks
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$stageRoot = if ($Configuration -eq 'Debug') { Join-Path $repoRoot 'out\dev' } else { Join-Path $repoRoot 'out\release' }
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

try {
    if ($Build) {
        & (Join-Path $PSScriptRoot 'build-driver.ps1') -Configuration $Configuration
        & (Join-Path $PSScriptRoot 'build-tools.ps1') -Configuration $Configuration
    }

    Write-Host '=== Install ==='
    & (Join-Path $PSScriptRoot 'install-driver.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw 'install-driver.ps1 failed.'
    }

    Write-Host ''
    Write-Host '=== Smoke ==='
    & (Join-Path $PSScriptRoot 'smoke-test.ps1') -Configuration $Configuration -IncludeRuntimeVerifiers
    if ($LASTEXITCODE -ne 0) {
        throw 'smoke-test.ps1 failed.'
    }

    if (-not (Test-Path $securityVerifier)) {
        throw "SecurityAutoVerify.exe not found at $securityVerifier."
    }

    Write-Host ''
    Write-Host '=== Security ==='
    & $securityVerifier
    if ($LASTEXITCODE -ne 0) {
        throw 'SecurityAutoVerify.exe failed.'
    }

    if (-not $SkipTransitionChecks) {
        Write-Host ''
        Write-Host '=== Transitions ==='
        & (Join-Path $PSScriptRoot 'transition-check.ps1') -Configuration $Configuration
        if ($LASTEXITCODE -ne 0) {
            throw 'transition-check.ps1 failed.'
        }
    }

    Write-Host ''
    Write-Host 'Release check PASS.'
}
finally {
    if (-not $KeepInstalled) {
        Write-Host ''
        Write-Host '=== Rollback ==='
        try {
            & (Join-Path $PSScriptRoot 'uninstall-driver.ps1')
        } catch {
            Write-Warning "Rollback failed: $($_.Exception.Message)"
        }
    }
}
