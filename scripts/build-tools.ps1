[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$legacyBatch = Join-Path $repoRoot 'build_usermode.bat'
$packageScript = Join-Path $PSScriptRoot 'package-bundle.ps1'
$profileRoot = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev\tools'
} else {
    Join-Path $repoRoot 'out\release\tools'
}
$profileSegment = if ($Configuration -eq 'Debug') { 'dev' } else { 'release' }

if (-not (Test-Path -LiteralPath $legacyBatch)) {
    throw "Tool batch entrypoint not found: $legacyBatch"
}

Write-Host "Building tools via batch entrypoint: $legacyBatch"
& cmd.exe /d /c "`"$legacyBatch`" $Configuration"
if ($LASTEXITCODE -ne 0) {
    throw "Tool batch build failed with exit code $LASTEXITCODE."
}

Get-ChildItem -LiteralPath $profileRoot -Filter *.obj -File -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host "Tool output directory: $profileRoot"
Get-ChildItem -LiteralPath $profileRoot -Filter *.exe | Select-Object Name, Length

Start-Sleep -Milliseconds 250

& powershell -NoProfile -ExecutionPolicy Bypass -File $packageScript `
    -Configuration $Configuration `
    -BundleName "GaYmXboxFilter-authoritative-$profileSegment-current" `
    -SkipIfInputsMissing
if ($LASTEXITCODE -ne 0) {
    throw "Authoritative bundle refresh failed with exit code $LASTEXITCODE."
}
