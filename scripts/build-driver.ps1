[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$legacyBatch = Join-Path $repoRoot 'build_driver.bat'
$signScript = Join-Path $PSScriptRoot 'sign-driver.ps1'
$packageScript = Join-Path $PSScriptRoot 'package-bundle.ps1'
$lowerStageRoot = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev\driver'
} else {
    Join-Path $repoRoot 'out\release\driver'
}
$upperStageRoot = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev\upper'
} else {
    Join-Path $repoRoot 'out\release\upper'
}
$staleLowerStage = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev\lower-driver'
} else {
    Join-Path $repoRoot 'out\release\lower-driver'
}
$profileSegment = if ($Configuration -eq 'Debug') { 'dev' } else { 'release' }
if (-not (Test-Path -LiteralPath $legacyBatch)) {
    throw "Driver batch entrypoint not found: $legacyBatch"
}

Write-Host "Building driver packages via batch entrypoint: $legacyBatch"
& cmd.exe /d /c "`"$legacyBatch`" $Configuration"
if ($LASTEXITCODE -ne 0) {
    throw "Driver batch build failed with exit code $LASTEXITCODE."
}

& powershell -NoProfile -ExecutionPolicy Bypass -File $signScript -Configuration $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "Driver signing step failed with exit code $LASTEXITCODE."
}

if (Test-Path -LiteralPath $staleLowerStage) {
    Remove-Item -LiteralPath $staleLowerStage -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $lowerStageRoot, $upperStageRoot | Out-Null

Write-Host ''
Write-Host "Driver output directory: $lowerStageRoot"
Get-ChildItem -LiteralPath $lowerStageRoot -File | Select-Object Name, Length
Write-Host "Upper driver staging directory: $upperStageRoot"
Get-ChildItem -LiteralPath $upperStageRoot -File | Select-Object Name, Length

& powershell -NoProfile -ExecutionPolicy Bypass -File $packageScript `
    -Configuration $Configuration `
    -BundleName "GaYmXboxFilter-authoritative-$profileSegment-current" `
    -SkipIfInputsMissing
if ($LASTEXITCODE -ne 0) {
    throw "Authoritative bundle refresh failed with exit code $LASTEXITCODE."
}
