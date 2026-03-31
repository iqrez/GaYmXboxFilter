[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [int]$DeviceIndex = 0,

    [switch]$RunAutoVerify,

    [switch]$LaunchXInputCheck
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$stageRoot = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev'
} else {
    Join-Path $repoRoot 'out\release'
}
$installStatePath = Join-Path $repoRoot 'out\install-driver-state.json'
$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'

function Clear-InstallState {
    if (Test-Path -LiteralPath $installStatePath) {
        Remove-Item -LiteralPath $installStatePath -Force
    }
}

function Find-ConnectedInstanceId {
    param(
        [string]$TargetHardwareId
    )

    $deviceOutput = & $pnputil /enum-devices /connected /class HIDClass 2>&1
    if ($LASTEXITCODE -ne 0) {
        return $null
    }

    $instanceMatch = $deviceOutput |
        Select-String '^\s*Instance ID:\s*(.+)$' |
        Where-Object {
            $_.Matches[0].Groups[1].Value.Trim().StartsWith($TargetHardwareId, [StringComparison]::OrdinalIgnoreCase)
        } |
        Select-Object -First 1

    if (-not $instanceMatch) {
        return $null
    }

    return $instanceMatch.Matches[0].Groups[1].Value.Trim()
}

function Test-InstanceRequiresReboot {
    param(
        [string]$TargetInstanceId
    )

    if (-not $TargetInstanceId) {
        return $true
    }

    $propertyOutput = & $pnputil /enum-devices /instanceid $TargetInstanceId /properties 2>&1
    if ($LASTEXITCODE -ne 0) {
        return $true
    }

    $propertyText = $propertyOutput -join [Environment]::NewLine
    return $propertyText -match 'DEVPKEY_Device_IsRebootRequired \[Boolean\]:\s*\r?\n\s*TRUE'
}

function Get-CurrentOverrideState {
    param(
        [string]$CliPath
    )

    $statusOutput = & $CliPath status 2>&1
    $exitCode = $LASTEXITCODE
    $statusText = $statusOutput -join [Environment]::NewLine
    $overrideOn = $statusText -match 'Override:ON'

    return [pscustomobject]@{
        ExitCode   = $exitCode
        Text       = $statusText
        OverrideOn = $overrideOn
    }
}

function Invoke-OverrideRecovery {
    param(
        [string]$CliPath,
        [int]$DeviceIndex
    )

    $initialState = Get-CurrentOverrideState -CliPath $CliPath
    if ($initialState.ExitCode -ne 0) {
        throw "GaYmCLI status failed during preflight recovery check.`n$($initialState.Text)"
    }

    if (-not $initialState.OverrideOn) {
        return
    }

    Write-Warning "Detected stale Override:ON state before verification. Attempting recovery with 'GaYmCLI off $DeviceIndex'."
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        & $CliPath off $DeviceIndex
        if ($LASTEXITCODE -eq 0) {
            break
        }

        if ($attempt -eq 5) {
            throw "GaYmCLI off $DeviceIndex failed during preflight recovery."
        }

        Start-Sleep -Seconds 2
    }

    Start-Sleep -Seconds 2
    $recoveredState = Get-CurrentOverrideState -CliPath $CliPath
    if ($recoveredState.ExitCode -ne 0) {
        throw "GaYmCLI status failed after preflight recovery.`n$($recoveredState.Text)"
    }
    if ($recoveredState.OverrideOn) {
        throw "Override remained ON after recovery attempt. Clear live state before rerunning smoke-test.ps1."
    }
}

$clientLib = Join-Path $stageRoot 'client\gaym_client.lib'
$gaYmCli = Join-Path $stageRoot 'tools\GaYmCLI.exe'
$gaYmFeeder = Join-Path $stageRoot 'tools\GaYmFeeder.exe'
$autoVerify = Join-Path $stageRoot 'tools\AutoVerify.exe'
$autoVerifyState = Join-Path $stageRoot 'verification\autoverify-state.json'
$xinputCheck = Join-Path $stageRoot 'tools\XInputCheck.exe'

foreach ($path in @($clientLib, $gaYmCli, $gaYmFeeder)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required staged artifact not found: $path"
    }
}
if ($RunAutoVerify -and -not (Test-Path -LiteralPath $autoVerify)) {
    throw "AutoVerify.exe was requested but is not staged at $autoVerify"
}
if ($LaunchXInputCheck -and -not (Test-Path -LiteralPath $xinputCheck)) {
    throw "XInputCheck.exe was requested but is not staged at $xinputCheck"
}

if (Test-Path -LiteralPath $installStatePath) {
    try {
        $installState = Get-Content -LiteralPath $installStatePath -Raw | ConvertFrom-Json
    } catch {
        throw "Install state file is unreadable: $installStatePath"
    }

    if ($installState.phase -eq 'resume-after-install-reboot') {
        $currentInstanceId = Find-ConnectedInstanceId -TargetHardwareId $installState.targetHardwareId
        if (Test-InstanceRequiresReboot -TargetInstanceId $currentInstanceId) {
            throw "Driver install is pending reboot for $($installState.targetHardwareId). Reboot once, reconnect the controller, rerun install-driver.ps1, then rerun smoke-test.ps1."
        }

        Clear-InstallState
        Write-Warning "Ignoring stale install reboot checkpoint at $installStatePath because the live device no longer reports reboot-required state."
    }
}

Write-Host "Using staged client library: $clientLib"
Write-Host "Using staged CLI: $gaYmCli"
Write-Host "Using staged feeder: $gaYmFeeder"
if (Test-Path -LiteralPath $autoVerify) {
    Write-Host "Using staged AutoVerify: $autoVerify"
}
if (Test-Path -LiteralPath $xinputCheck) {
    Write-Host "Using staged XInputCheck: $xinputCheck"
}
Write-Host 'Observation/control preference: upper control path first, diagnostic lower only as fallback.'
Write-Host ''

Push-Location $stageRoot
try {
    Invoke-OverrideRecovery -CliPath $gaYmCli -DeviceIndex $DeviceIndex

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

    if ($LaunchXInputCheck) {
        Write-Host ''
        Write-Host 'Launching XInputCheck for interactive XInput observation...'
        Start-Process -FilePath $xinputCheck | Out-Null
    }

    if ($RunAutoVerify) {
        Write-Host ''
        Write-Host 'Running AutoVerify...'
        & $autoVerify
        if ($LASTEXITCODE -ne 0) {
            throw "AutoVerify failed with exit code $LASTEXITCODE."
        }
        if (Test-Path -LiteralPath $autoVerifyState) {
            Write-Host "AutoVerify state: $autoVerifyState"
        }
    }

    Write-Host ''
    Write-Host 'Smoke test passed.'
    Write-Host 'Recommended interactive proof path: GaYmCLI/GaYmFeeder + XInputCheck + AutoVerify.'
    Write-Host 'QuickVerify and XInputAutoVerify are diagnostic-only and are not staged as curated pass/fail tools on this machine.'
}
finally {
    Pop-Location
}
