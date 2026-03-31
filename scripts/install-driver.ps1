[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$HardwareId = 'HID\VID_045E&PID_02FF&IG_00',
    [string]$InstanceId,
    [string]$DriverInf,
    [string]$UpperDriverInf
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$profileSegment = if ($Configuration -eq 'Debug') { 'dev' } else { 'release' }
$statePath = Join-Path $repoRoot 'out\install-driver-state.json'
if (-not $DriverInf) {
    $DriverInf = Join-Path $repoRoot ("out\" + $profileSegment + '\driver\GaYmFilter.inf')
}
if (-not $UpperDriverInf) {
    $UpperDriverInf = Join-Path $repoRoot ("out\" + $profileSegment + '\upper\GaYmXInputFilter.inf')
}

function Resolve-DriverInfPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InputPath,

        [Parameter(Mandatory = $true)]
        [string]$InfName
    )

    if (-not (Test-Path -LiteralPath $InputPath)) {
        $parentPath = Split-Path -Parent $InputPath
        if (-not $parentPath -or -not (Test-Path -LiteralPath $parentPath)) {
            return $null
        }

        $candidatePaths = @(
            (Join-Path $parentPath $InfName),
            (Join-Path (Join-Path $parentPath 'package') $InfName)
        )

        foreach ($candidatePath in $candidatePaths) {
            if (Test-Path -LiteralPath $candidatePath) {
                return (Get-Item -LiteralPath $candidatePath).FullName
            }
        }

        return $null
    }

    $inputItem = Get-Item -LiteralPath $InputPath
    if (-not $inputItem.PSIsContainer) {
        if ($inputItem.Extension -ine '.inf') {
            throw "Driver package path must be an INF or directory: $InputPath"
        }

        return $inputItem.FullName
    }

    $candidatePaths = @(
        (Join-Path $inputItem.FullName $InfName),
        (Join-Path (Join-Path $inputItem.FullName 'package') $InfName)
    )

    foreach ($candidatePath in $candidatePaths) {
        if (Test-Path -LiteralPath $candidatePath) {
            return (Get-Item -LiteralPath $candidatePath).FullName
        }
    }

    $recursiveMatch = Get-ChildItem -LiteralPath $inputItem.FullName -Recurse -File -Filter $InfName -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($recursiveMatch) {
        return $recursiveMatch.FullName
    }

    throw "Unable to locate $InfName under $InputPath."
}

function Save-InstallState {
    param(
        [string]$Phase,
        [bool]$UpperInstalled,
        [bool]$LowerInstalled
    )

    $state = [ordered]@{
        schema           = 1
        phase            = $Phase
        targetHardwareId = $HardwareId
        upperPackage     = 'GaYmXInputFilter'
        lowerPackage     = 'GaYmFilter'
        upperInstalled   = $UpperInstalled
        lowerInstalled   = $LowerInstalled
        createdUtc       = [DateTime]::UtcNow.ToString('o')
    }

    $stateDirectory = Split-Path -Parent $statePath
    if (-not (Test-Path -LiteralPath $stateDirectory)) {
        New-Item -ItemType Directory -Force -Path $stateDirectory | Out-Null
    }

    $state | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding ASCII
}

function Clear-InstallState {
    if (Test-Path -LiteralPath $statePath) {
        Remove-Item -LiteralPath $statePath -Force
    }
}

function Invoke-DriverInstall {
    param(
        [string]$InfPath,
        [string]$Label
    )

    $output = & $pnputil /add-driver $InfPath /install 2>&1
    $exitCode = $LASTEXITCODE
    $text = $output -join [Environment]::NewLine
    $rebootRequired = $text -match 'System reboot is needed to complete install operations!'
    $success =
        $text -match 'Driver package added successfully\.' -or
        $text -match 'Driver package is already imported\.' -or
        $text -match 'Published Name:\s+oem\d+\.inf'

    if ($exitCode -ne 0 -and -not ($success -and $rebootRequired)) {
        throw ("pnputil failed to add/install the $Label driver package.`n" + $text)
    }

    return [pscustomobject]@{
        Output         = $output
        ExitCode       = $exitCode
        RebootRequired = $rebootRequired
        Text           = $text
    }
}

function Find-ConnectedInstanceId {
    param(
        [string]$TargetHardwareId,
        [int]$Attempts = 5,
        [int]$DelaySeconds = 2
    )

    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        $deviceOutput = & $pnputil /enum-devices /connected /class HIDClass 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw 'pnputil failed while enumerating connected HIDClass devices.'
        }

        $instanceMatch = $deviceOutput |
            Select-String '^\s*Instance ID:\s*(.+)$' |
            Where-Object {
                $_.Matches[0].Groups[1].Value.Trim().StartsWith($TargetHardwareId, [StringComparison]::OrdinalIgnoreCase)
            } |
            Select-Object -First 1

        if ($instanceMatch) {
            return $instanceMatch.Matches[0].Groups[1].Value.Trim()
        }

        if ($attempt -lt $Attempts) {
            Start-Sleep -Seconds $DelaySeconds
        }
    }

    return $null
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run install-driver.ps1 from an elevated PowerShell prompt.'
}

$requestedDriverInf = $DriverInf
$resolvedDriverInf = Resolve-DriverInfPath -InputPath $DriverInf -InfName 'GaYmFilter.inf'
if (-not $resolvedDriverInf) {
    throw "Driver INF not found: $requestedDriverInf"
}

$resolvedUpperDriverInf = $null
$resolvedUpperDriverInf = Resolve-DriverInfPath -InputPath $UpperDriverInf -InfName 'GaYmXInputFilter.inf'
$expectUpper = [bool]$resolvedUpperDriverInf

$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$upperInstall = $null
$lowerInstall = $null

if ($expectUpper) {
    $upperInstall = Invoke-DriverInstall -InfPath $resolvedUpperDriverInf -Label 'upper'

    Write-Host $upperInstall.Text
    Write-Host ''
    Write-Host "Upper driver package staged: $resolvedUpperDriverInf"
    Write-Host ''
} else {
    Write-Host "Upper driver package not present at $UpperDriverInf; skipping upper install."
    Write-Host ''
}

$lowerInstall = Invoke-DriverInstall -InfPath $resolvedDriverInf -Label 'lower'
$rebootRequired = (($upperInstall -ne $null) -and $upperInstall.RebootRequired) -or $lowerInstall.RebootRequired

if (-not $InstanceId) {
    $InstanceId = Find-ConnectedInstanceId -TargetHardwareId $HardwareId
    if (-not $InstanceId) {
        throw "No connected device matches hardware ID $HardwareId."
    }
}

if ($rebootRequired) {
    Save-InstallState -Phase 'resume-after-install-reboot' -UpperInstalled:$expectUpper -LowerInstalled:$true
    Write-Host $lowerInstall.Text
    Write-Host ''
    Write-Warning "Windows reports that driver installation is pending reboot. Reboot once, reconnect the controller, and rerun install-driver.ps1 to verify the live stack. State saved to $statePath."
    return
}

$restartOutput = & $pnputil /restart-device $InstanceId 2>&1
$restartExitCode = $LASTEXITCODE
$restartText = $restartOutput -join [Environment]::NewLine

if (($restartExitCode -ne 0) -and ($restartText -match 'System reboot is needed to complete configuration operations!')) {
    Save-InstallState -Phase 'resume-after-install-reboot' -UpperInstalled:$expectUpper -LowerInstalled:$true
    Write-Host $lowerInstall.Text
    Write-Host ''
    Write-Host "Device Instance: $InstanceId"
    Write-Host ''
    Write-Host 'Restart output:'
    Write-Host $restartText
    Write-Host ''
    Write-Warning "Windows requires a reboot to finish reconfiguring $InstanceId. Reboot once, reconnect the controller, and rerun install-driver.ps1 to verify the live stack. State saved to $statePath."
    return
}

$stackOutput = & $pnputil /enum-devices /instanceid $InstanceId /services /stack /drivers 2>&1
if ($LASTEXITCODE -ne 0) {
    throw ("pnputil failed while querying the live stack for $InstanceId.`n" + ($stackOutput -join [Environment]::NewLine))
}

$stackText = $stackOutput -join [Environment]::NewLine
$hasGaYmXInputFilter = $stackText -match '(?im)^\s*(?:Stack:\s*)?GaYmXInputFilter\s*$'
$hasGaYmFilter = $stackText -match '(?im)^\s*(?:Stack:\s*)?GaYmFilter\s*$'
$hasHidUsb = $stackText -match '(?im)^\s*(?:Stack:\s*)?HidUsb\s*$'

Write-Host $lowerInstall.Text
Write-Host ''
Write-Host "Device Instance: $InstanceId"
Write-Host ''
Write-Host 'Restart output:'
Write-Host $restartText
Write-Host ''
Write-Host 'Live stack:'
Write-Host $stackText
Write-Host ''

if (-not $hasGaYmFilter) {
    if (($restartOutput -join [Environment]::NewLine) -match 'pending system reboot') {
        throw "Windows reports that $InstanceId is pending a system reboot. Reboot once, reconnect the controller, and rerun install-driver.ps1 to verify the live stack."
    }

    if ($restartExitCode -ne 0) {
        throw "pnputil failed to restart $InstanceId before stack verification."
    }

    throw "GaYmFilter is not present in the live stack for $InstanceId."
}

if ($expectUpper -and -not $hasGaYmXInputFilter) {
    throw "GaYmXInputFilter is not present in the live stack for $InstanceId."
}

if (-not $hasHidUsb) {
    throw "HidUsb is not present in the live stack for $InstanceId. The filter is not attached on the supported HID-child path."
}

if ($restartExitCode -ne 0) {
    Write-Warning 'pnputil reported that additional reboot or device reconfiguration may still be pending, but the verified live stack already includes GaYmFilter.'
}

Clear-InstallState

if ($expectUpper) {
    Write-Host 'GaYmXInputFilter and GaYmFilter are present on the supported HID-child stack alongside HidUsb.'
} else {
    Write-Host 'GaYmFilter is present on the supported HID-child stack alongside HidUsb.'
}
