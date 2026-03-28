[CmdletBinding()]
param(
    [ValidateSet('Release')]
    [string]$Configuration = 'Release',
    [string]$OutputName = 'GaYmXboxFilter-release-bundle.zip'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$stageRoot = Join-Path $repoRoot 'out\release'
$bundleRoot = Join-Path $stageRoot 'bundle'
$bundleDocs = Join-Path $bundleRoot 'docs'
$bundleScripts = Join-Path $bundleRoot 'scripts'
$bundleDriversUpper = Join-Path $bundleRoot 'driver-upper'
$bundleDriversLower = Join-Path $bundleRoot 'driver-lower'
$bundleTools = Join-Path $bundleRoot 'tools'
$zipPath = Join-Path $stageRoot $OutputName

function Copy-TreeContents {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,
        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Copy-Item -Path (Join-Path $Source '*') -Destination $Destination -Recurse -Force
}

& (Join-Path $PSScriptRoot 'build-driver.ps1') -Configuration $Configuration
& (Join-Path $PSScriptRoot 'build-tools.ps1') -Configuration $Configuration -ToolProfile ReleaseBundle

if (Test-Path $bundleRoot) {
    Remove-Item -Path $bundleRoot -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $bundleRoot, $bundleDocs, $bundleScripts, $bundleDriversUpper, $bundleDriversLower, $bundleTools | Out-Null

Copy-Item -Path (Join-Path $repoRoot 'README.md') -Destination $bundleRoot -Force
Copy-TreeContents -Source (Join-Path $repoRoot 'docs') -Destination $bundleDocs
Copy-TreeContents -Source (Join-Path $stageRoot 'driver-upper') -Destination $bundleDriversUpper
Copy-TreeContents -Source (Join-Path $stageRoot 'driver-lower') -Destination $bundleDriversLower
Copy-TreeContents -Source (Join-Path $stageRoot 'tools') -Destination $bundleTools

foreach ($scriptName in @(
    'install-driver.ps1',
    'uninstall-driver.ps1',
    'smoke-test.ps1',
    'release-check.ps1'
)) {
    Copy-Item -Path (Join-Path $PSScriptRoot $scriptName) -Destination (Join-Path $bundleScripts $scriptName) -Force
}

$commit = (git -C $repoRoot rev-parse --short HEAD).Trim()
$tag = (git -C $repoRoot describe --tags --exact-match 2>$null)
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($tag)) {
    $tag = 'unreleased'
}

$manifest = @"
GaYmXboxFilter Release Bundle
Configuration : $Configuration
Commit        : $commit
Tag           : $tag
Generated     : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')

Contents
- driver-upper
- driver-lower
- tools (release bundle profile)
- docs
- scripts/install-driver.ps1
- scripts/uninstall-driver.ps1
- scripts/smoke-test.ps1
- scripts/release-check.ps1

Notes
- release bundle excludes deep sniffers and maintainer-only packet probes
- dev-only jitter support remains disabled in non-debug driver builds
"@

Set-Content -Path (Join-Path $bundleRoot 'BUNDLE-MANIFEST.txt') -Value $manifest -Encoding ASCII

if (Test-Path $zipPath) {
    Remove-Item -Path $zipPath -Force
}

Compress-Archive -Path (Join-Path $bundleRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal

Write-Host "Release bundle staged at: $bundleRoot"
Write-Host "Release bundle zip: $zipPath"
