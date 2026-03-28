[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Build
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run install-driver.ps1 from an elevated PowerShell session.'
    }
}

function Get-HidChildInstanceId {
    param(
        [string]$HardwareId = 'HID\VID_045E&PID_02FF&IG_00'
    )

    $devices = Get-PnpDevice -PresentOnly -Class HIDClass -ErrorAction Stop
    $match = $devices | Where-Object { $_.InstanceId -like "$HardwareId*" } | Select-Object -First 1
    if ($match) {
        return $match.InstanceId
    }

    return $null
}

function Get-LiveStackText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstanceId
    )

    $pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
    $stackOutput = & $pnputil /enum-devices /instanceid $InstanceId /stack /drivers 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw ("pnputil failed while querying the live stack for $InstanceId.`n" + ($stackOutput -join [Environment]::NewLine))
    }

    return ($stackOutput -join [Environment]::NewLine)
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

Assert-Administrator

if ($Build) {
    & (Join-Path $PSScriptRoot 'build-driver.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw 'Driver build failed.'
    }
}

$stageRoot = if ($Configuration -eq 'Debug') { Join-Path $repoRoot 'out\dev' } else { Join-Path $repoRoot 'out\release' }
$lowerInf = Join-Path $stageRoot 'driver-lower\package\GaYmFilter.inf'
$upperInf = Join-Path $stageRoot 'driver-upper\package\GaYmXboxFilter.inf'

foreach ($path in @($lowerInf, $upperInf)) {
    if (-not (Test-Path $path)) {
        throw "Driver package not found: $path. Run scripts\\build-driver.ps1 first or use -Build."
    }
}

$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$instanceId = Get-HidChildInstanceId
if (-not $instanceId) {
    throw 'The supported HID child `HID\VID_045E&PID_02FF&IG_00` is not currently present. Connect the controller and wait for full enumeration before installing.'
}

function Invoke-PnpAddDriver {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InfPath,
        [Parameter(Mandatory = $true)]
        [string]$FailureMessage
    )

    $output = & $pnputil /add-driver $InfPath /install 2>&1
    $exitCode = $LASTEXITCODE
    $outputText = $output -join [Environment]::NewLine

    Write-Host $outputText

    $succeeded = $outputText -match 'Driver package added successfully' -or
        $outputText -match 'Driver package is up-to-date on device'

    if ($succeeded -or $exitCode -eq 0 -or $exitCode -eq 3010) {
        return ($outputText -match 'System reboot is needed')
    }

    throw $FailureMessage
}

Write-Host '=== Installing lower HID-child filter ==='
$rebootRequired = Invoke-PnpAddDriver -InfPath $lowerInf -FailureMessage 'Lower filter install failed.'

Write-Host ''
Write-Host '=== Installing upper HID-child filter ==='
$rebootRequired = (Invoke-PnpAddDriver -InfPath $upperInf -FailureMessage 'Upper filter install failed. The lower filter may already be present; use scripts\uninstall-driver.ps1 if you need to roll back cleanly.') -or $rebootRequired

Start-Sleep -Seconds 2
$stackText = Get-LiveStackText -InstanceId $instanceId
$stackNames = Get-StackDriverNames -StackText $stackText

Write-Host ''
Write-Host 'Install requested for the supported hybrid stack.'
Write-Host "Target instance: $instanceId"
Write-Host ("Normalized stack: {0}" -f ($stackNames -join ' -> '))
Write-Host ''
Write-Host 'Live stack snapshot:'
Write-Host $stackText
Write-Host ''

if (-not (Test-SupportedHybridStack -StackNames $stackNames)) {
    Write-Warning 'The requested packages were installed, but the live hybrid stack is not fully visible yet. If Windows reports pending configuration, reboot before trusting runtime results.'
} else {
    Write-Host 'The live stack already shows the supported hybrid path.'
}

if ($rebootRequired) {
    Write-Warning 'Windows reported that a reboot is required to finish applying one or both driver packages.'
}
