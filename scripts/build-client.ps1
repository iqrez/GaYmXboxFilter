[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$legacyBatch = Join-Path $repoRoot 'build_client.bat'
$packageScript = Join-Path $PSScriptRoot 'package-bundle.ps1'
$profileRoot = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev\client'
} else {
    Join-Path $repoRoot 'out\release\client'
}
$profileSegment = if ($Configuration -eq 'Debug') { 'dev' } else { 'release' }

if (-not (Test-Path -LiteralPath $legacyBatch)) {
    throw "Client batch entrypoint not found: $legacyBatch"
}

Write-Host "Building client via batch fallback: $legacyBatch"
& cmd.exe /d /c "`"$legacyBatch`" $Configuration"
if ($LASTEXITCODE -ne 0) {
    throw "Client batch build failed with exit code $LASTEXITCODE."
}

Write-Host ''
Write-Host "Client output directory: $profileRoot"
Get-ChildItem -LiteralPath $profileRoot -Filter *.lib | Select-Object Name, Length

& powershell -NoProfile -ExecutionPolicy Bypass -File $packageScript `
    -Configuration $Configuration `
    -BundleName "GaYmXboxFilter-authoritative-$profileSegment-current" `
    -SkipIfInputsMissing
if ($LASTEXITCODE -ne 0) {
    throw "Authoritative bundle refresh failed with exit code $LASTEXITCODE."
}
