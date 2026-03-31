[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [int]$DeviceIndex = 0
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$stageRoot = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev'
} else {
    Join-Path $repoRoot 'out\release'
}
$installStatePath = Join-Path $repoRoot 'out\install-driver-state.json'

$clientLib = Join-Path $stageRoot 'client\gaym_client.lib'
$gaYmCli = Join-Path $stageRoot 'tools\GaYmCLI.exe'
$gaYmFeeder = Join-Path $stageRoot 'tools\GaYmFeeder.exe'

foreach ($path in @($clientLib, $gaYmCli, $gaYmFeeder)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required staged artifact not found: $path"
    }
}

if (Test-Path -LiteralPath $installStatePath) {
    try {
        $installState = Get-Content -LiteralPath $installStatePath -Raw | ConvertFrom-Json
    } catch {
        throw "Install state file is unreadable: $installStatePath"
    }

    if ($installState.phase -eq 'resume-after-install-reboot') {
        throw "Driver install is pending reboot for $($installState.targetHardwareId). Reboot once, reconnect the controller, rerun install-driver.ps1, then rerun smoke-test.ps1."
    }
}

Write-Host "Using staged client library: $clientLib"
Write-Host "Using staged CLI: $gaYmCli"
Write-Host "Using staged feeder: $gaYmFeeder"
Write-Host ''

Push-Location $stageRoot
try {
    Write-Host 'Running GaYmCLI status...'
    & $gaYmCli status
    if ($LASTEXITCODE -ne 0) {
        throw "GaYmCLI status failed with exit code $LASTEXITCODE."
    }

    Write-Host ''
    Write-Host "Running GaYmCLI test $DeviceIndex..."
    & $gaYmCli test $DeviceIndex
    if ($LASTEXITCODE -ne 0) {
        throw "GaYmCLI test failed with exit code $LASTEXITCODE."
    }

    Write-Host ''
    Write-Host 'Smoke test passed.'
}
finally {
    Pop-Location
}
