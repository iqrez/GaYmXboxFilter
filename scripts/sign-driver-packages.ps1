[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$signScript = Join-Path $PSScriptRoot 'sign-driver.ps1'
if (-not (Test-Path -LiteralPath $signScript)) {
    throw "sign-driver.ps1 not found: $signScript"
}

& powershell -NoProfile -ExecutionPolicy Bypass -File $signScript -Configuration $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "sign-driver.ps1 failed with exit code $LASTEXITCODE."
}
