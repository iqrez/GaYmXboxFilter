[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$BuildTools,
    [switch]$IncludeRuntimeVerifiers
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common-paths.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $Configuration
$cli = Get-GaYmToolPath -Layout $layout -Name 'GaYmCLI.exe'
$joyVerify = Get-GaYmToolPath -Layout $layout -Name 'JoyAutoVerify.exe'
$hybridVerify = Get-GaYmToolPath -Layout $layout -Name 'HybridAutoVerify.exe'
$installStatePath = New-GaYmStatePath -Layout $layout -LeafName 'install-driver-state.json'
$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'

if ($BuildTools) {
    $buildToolsScript = Join-Path $PSScriptRoot 'build-tools.ps1'
    if (-not (Test-Path $buildToolsScript)) {
        throw 'This extracted bundle does not include build-tools.ps1. Use the bundled tools directly or rebuild from the full repo.'
    }

    & $buildToolsScript -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw 'Tool build failed.'
    }

    $layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $Configuration
    $cli = Get-GaYmToolPath -Layout $layout -Name 'GaYmCLI.exe'
    $joyVerify = Get-GaYmToolPath -Layout $layout -Name 'JoyAutoVerify.exe'
    $hybridVerify = Get-GaYmToolPath -Layout $layout -Name 'HybridAutoVerify.exe'
}

if (-not (Test-Path $cli)) {
    if ($layout.Mode -eq 'Bundle') {
        throw "GaYmCLI.exe not found inside the extracted bundle: $cli. Re-extract the bundle or regenerate it with scripts\\package-bundle.ps1 from the repo."
    }

    throw "GaYmCLI.exe not found at $cli. Run scripts\\build-tools.ps1 first or use -BuildTools."
}

function Get-CurrentBootTimeUtc {
    return (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime.ToUniversalTime()
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

function Remove-InstallState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (Test-Path $Path) {
        Remove-Item -Path $Path -Force
    }
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

function Get-PendingDeclarativeFilterSummary {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstanceId
    )

    $filtersRoot = Join-Path 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum' $InstanceId
    $filtersRoot = Join-Path $filtersRoot 'Filters'
    if (-not (Test-Path $filtersRoot)) {
        return $null
    }

    $parts = New-Object 'System.Collections.Generic.List[string]'
    foreach ($subKeyName in @('*Upper', '*Lower')) {
        $subKeyPath = Join-Path $filtersRoot $subKeyName
        if (-not (Test-Path $subKeyPath)) {
            continue
        }

        $properties = Get-ItemProperty -Path $subKeyPath
        $filterNames = $properties.PSObject.Properties |
            Where-Object { $_.Name -notlike 'PS*' } |
            Select-Object -ExpandProperty Name
        if ($filterNames) {
            $parts.Add(("{0}={1}" -f $subKeyName.TrimStart('*'), ($filterNames -join ', ')))
        }
    }

    if ($parts.Count -eq 0) {
        return $null
    }

    return ($parts -join '; ')
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

function Invoke-GaYmStatus {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('upper', 'lower')]
        [string]$Target
    )

    $previousTarget = $env:GAYM_CONTROL_TARGET
    $env:GAYM_CONTROL_TARGET = $Target
    try {
        $output = & $cli status 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        if ($null -eq $previousTarget) {
            Remove-Item Env:GAYM_CONTROL_TARGET -ErrorAction SilentlyContinue
        } else {
            $env:GAYM_CONTROL_TARGET = $previousTarget
        }
    }

    if ($exitCode -ne 0) {
        throw "GaYmCLI status failed for target '$Target'."
    }

    return ($output -join [Environment]::NewLine)
}

function Invoke-GaYmBoundedTest {
    $previousTarget = $env:GAYM_CONTROL_TARGET
    $env:GAYM_CONTROL_TARGET = 'upper'
    try {
        & $cli test 0
        $exitCode = $LASTEXITCODE
    } finally {
        if ($null -eq $previousTarget) {
            Remove-Item Env:GAYM_CONTROL_TARGET -ErrorAction SilentlyContinue
        } else {
            $env:GAYM_CONTROL_TARGET = $previousTarget
        }
    }

    if ($exitCode -ne 0) {
        throw 'GaYmCLI test 0 failed.'
    }
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

$pendingInstallState = Read-InstallState -Path $installStatePath
if ($pendingInstallState) {
    $savedBootTimeUtc = [datetime]::Parse([string]$pendingInstallState.BootTimeUtc).ToUniversalTime()
    $currentBootTimeUtc = Get-CurrentBootTimeUtc
    if ($currentBootTimeUtc -gt $savedBootTimeUtc) {
        Remove-InstallState -Path $installStatePath
    } else {
        $pendingInstanceId = [string]$pendingInstallState.InstanceId
        if (-not $pendingInstanceId) {
            $pendingInstanceId = Get-HidChildInstanceId
        }

        $pendingStackOutput = & $pnputil /enum-devices /instanceid $pendingInstanceId /stack /drivers 2>&1
        if ($LASTEXITCODE -eq 0) {
            $pendingStackText = $pendingStackOutput -join [Environment]::NewLine
            $pendingStackNames = Get-StackDriverNames -StackText $pendingStackText
            $pendingRebootRequired = Test-DeviceRebootRequired -InstanceId $pendingInstanceId
            if ((Test-SupportedHybridStack -StackNames $pendingStackNames) -and -not $pendingRebootRequired) {
                Remove-InstallState -Path $installStatePath
            } else {
                throw ("install-driver.ps1 already reported a reboot-required boundary on this boot. Reboot before running smoke-test.ps1. Pending install state: {0}" -f $installStatePath)
            }
        } else {
            throw ("install-driver.ps1 already reported a reboot-required boundary on this boot. Reboot before running smoke-test.ps1. Pending install state: {0}" -f $installStatePath)
        }
    }
}

$instanceId = Get-HidChildInstanceId
$stackOutput = & $pnputil /enum-devices /instanceid $instanceId /stack /drivers 2>&1
if ($LASTEXITCODE -ne 0) {
    throw ("pnputil failed while querying the live stack for $instanceId.`n" + ($stackOutput -join [Environment]::NewLine))
}

$stackText = $stackOutput -join [Environment]::NewLine
$stackNames = Get-StackDriverNames -StackText $stackText
if (-not (Test-SupportedHybridStack -StackNames $stackNames)) {
    if (Test-DeviceRebootRequired -InstanceId $instanceId) {
        $pendingFilterSummary = Get-PendingDeclarativeFilterSummary -InstanceId $instanceId
        $details = if ($pendingFilterSummary) {
            " Pending declarative filters: $pendingFilterSummary."
        } else {
            ''
        }

        throw "The device is still in a reboot-required state after driver install. Reboot before running runtime verification.$details`n$stackText"
    }

    throw "The live stack is not the supported hybrid path for $instanceId.`n$stackText"
}

Write-Host '=== Live stack ==='
Write-Host ("Artifact mode: {0}" -f $layout.Mode)
Write-Host ("Normalized stack: {0}" -f ($stackNames -join ' -> '))
Write-Host $stackText
Write-Host ''

Write-Host '=== Upper status before test ==='
$upperStatusBefore = Invoke-GaYmStatus -Target 'upper'
Write-Host $upperStatusBefore
if ($upperStatusBefore -notmatch 'Layout:' -or $upperStatusBefore -notmatch 'Override:') {
    throw 'Upper status output is missing expected fields.'
}

Write-Host ''
Write-Host '=== Lower status before test ==='
$lowerStatusBefore = Invoke-GaYmStatus -Target 'lower'
Write-Host $lowerStatusBefore
if ($lowerStatusBefore -notmatch 'Layout:' -or $lowerStatusBefore -notmatch 'QueryBytes:') {
    throw 'Lower status output is missing expected fields.'
}

Write-Host ''
Write-Host '=== Bounded injection test ==='
Invoke-GaYmBoundedTest

Write-Host ''
Write-Host '=== Upper status after test ==='
$upperStatusAfter = Invoke-GaYmStatus -Target 'upper'
Write-Host $upperStatusAfter
if ($upperStatusAfter -notmatch 'Override:OFF') {
    throw 'Upper status did not return to Override:OFF.'
}

Write-Host ''
Write-Host '=== Lower status after test ==='
$lowerStatusAfter = Invoke-GaYmStatus -Target 'lower'
Write-Host $lowerStatusAfter
if ($lowerStatusAfter -notmatch 'Layout:' -or $lowerStatusAfter -notmatch 'QueryBytes:') {
    throw 'Lower status after test is missing expected fields.'
}

if ($IncludeRuntimeVerifiers) {
    foreach ($verifier in @($joyVerify, $hybridVerify)) {
        if (-not (Test-Path $verifier)) {
            throw "Verifier not found: $verifier"
        }
    }

    Write-Host ''
    Write-Host '=== JoyAutoVerify ==='
    & $joyVerify
    if ($LASTEXITCODE -ne 0) {
        throw 'JoyAutoVerify.exe failed.'
    }

    Write-Host ''
    Write-Host '=== HybridAutoVerify ==='
    & $hybridVerify
    if ($LASTEXITCODE -ne 0) {
        throw 'HybridAutoVerify.exe failed.'
    }
}
