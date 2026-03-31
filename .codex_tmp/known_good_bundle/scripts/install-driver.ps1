[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Build,
    [switch]$FailOnRebootRequired
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common-paths.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $Configuration
$packagePaths = Get-GaYmDriverPackagePaths -Layout $layout
$installStatePath = New-GaYmStatePath -Layout $layout -LeafName 'install-driver-state.json'

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

    $devices = Get-PnpDevice -Class HIDClass -ErrorAction Stop
    $match = $devices |
        Where-Object { $_.InstanceId -like "$HardwareId*" -and ($_.Present -eq $true -or $_.Status -eq 'OK') } |
        Select-Object -First 1
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

function Get-GaYmDriverRecords {
    $pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
    $lines = & $pnputil /enum-drivers 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw 'pnputil /enum-drivers failed.'
    }

    $blocks = @()
    $current = @()
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            if ($current.Count -gt 0) {
                $blocks += ,@($current)
                $current = @()
            }
            continue
        }
        $current += $line
    }

    if ($current.Count -gt 0) {
        $blocks += ,@($current)
    }

    $records = @()
    foreach ($block in $blocks) {
        $publishedName = $null
        $originalName = $null
        foreach ($line in $block) {
            if ($line -match '^\s*Published Name:\s*(.+)$') {
                $publishedName = $Matches[1].Trim()
            } elseif ($line -match '^\s*Original Name:\s*(.+)$') {
                $originalName = $Matches[1].Trim()
            }
        }

        if ($publishedName -and ($originalName -ieq 'GaYmXboxFilter.inf' -or $originalName -ieq 'GaYmFilter.inf')) {
            $records += [pscustomobject]@{
                PublishedName = $publishedName
                OriginalName = $originalName
            }
        }
    }

    return $records
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

function Write-InstallState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Configuration,
        [Parameter(Mandatory = $true)]
        [string]$InstanceId,
        [Parameter(Mandatory = $true)]
        [datetime]$BootTimeUtc
    )

    $stateDirectory = Split-Path -Parent $Path
    if (-not (Test-Path $stateDirectory)) {
        New-Item -ItemType Directory -Path $stateDirectory -Force | Out-Null
    }

    $state = [ordered]@{
        Version       = 1
        Phase         = 'AwaitingPostInstallReboot'
        Configuration = $Configuration
        InstanceId    = $InstanceId
        BootTimeUtc   = $BootTimeUtc.ToString('o')
        SavedAt       = (Get-Date).ToUniversalTime().ToString('o')
    }

    $state | ConvertTo-Json | Set-Content -Path $Path -Encoding ASCII
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
        [string[]]$StackNames
    )

    if ($null -eq $StackNames -or $StackNames.Count -eq 0) {
        return $false
    }

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

$currentBootTimeUtc = Get-CurrentBootTimeUtc
$pendingInstallState = Read-InstallState -Path $installStatePath
if ($pendingInstallState) {
    $savedBootTimeUtc = [datetime]::Parse([string]$pendingInstallState.BootTimeUtc).ToUniversalTime()
    if ($currentBootTimeUtc -gt $savedBootTimeUtc) {
        Remove-InstallState -Path $installStatePath
    }
}

if ($Build) {
    $buildScript = Join-Path $PSScriptRoot 'build-driver.ps1'
    if (-not (Test-Path $buildScript)) {
        throw 'This extracted bundle does not include build-driver.ps1. Build from the full repo or install directly from the bundled driver packages.'
    }

    & $buildScript -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw 'Driver build failed.'
    }

    $layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $Configuration
    $packagePaths = Get-GaYmDriverPackagePaths -Layout $layout
}

$lowerInf = $packagePaths.LowerInf
$upperInf = $packagePaths.UpperInf

foreach ($path in @($lowerInf, $upperInf)) {
    if (-not (Test-Path $path)) {
        if ($layout.Mode -eq 'Bundle') {
            throw "Driver package not found inside the extracted bundle: $path. Re-extract the bundle or regenerate it with scripts\\package-bundle.ps1 from the repo."
        }

        throw "Driver package not found: $path. Run scripts\\build-driver.ps1 first or use -Build."
    }
}

$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$driverRecordsBefore = Get-GaYmDriverRecords
$instanceId = Get-HidChildInstanceId
if (-not $instanceId) {
    throw 'The supported HID child `HID\VID_045E&PID_02FF&IG_00` is not currently present. Connect the controller and wait for full enumeration before installing.'
}

$preInstallStackText = Get-LiveStackText -InstanceId $instanceId
$preInstallStackNames = Get-StackDriverNames -StackText $preInstallStackText
if ($null -eq $preInstallStackNames) {
    $preInstallStackNames = @()
}
$stackAlreadySupported = Test-SupportedHybridStack -StackNames $preInstallStackNames

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

    $added = $outputText -match 'Driver package added successfully'
    $upToDate = $outputText -match 'Driver package is up-to-date on device'
    $succeeded = $added -or $upToDate

    if ($succeeded -or $exitCode -eq 0 -or $exitCode -eq 3010) {
        return [pscustomobject]@{
            RebootRequired = ($outputText -match 'System reboot is needed')
            Added = $added
            UpToDate = $upToDate
        }
    }

    if ($outputText -match 'root certificate which is not trusted' -or $outputText -match 'trust provider') {
        throw "$FailureMessage Run scripts\prepare-test-machine.ps1 first to import the bundled test certificates and verify testsigning state."
    }

    throw $FailureMessage
}

Write-Host '=== Installing lower HID-child filter ==='
$lowerResult = Invoke-PnpAddDriver -InfPath $lowerInf -FailureMessage 'Lower filter install failed.'
$rebootRequired = $lowerResult.RebootRequired

Write-Host ''
Write-Host '=== Installing upper HID-child filter ==='
$upperResult = Invoke-PnpAddDriver -InfPath $upperInf -FailureMessage 'Upper filter install failed. The lower filter may already be present; use scripts\uninstall-driver.ps1 if you need to roll back cleanly.'
$reportedRebootRequired = $lowerResult.RebootRequired -or $upperResult.RebootRequired

$stackText = $null
$stackNames = @()
$deviceRebootRequired = $false
$pendingFilterSummary = $null
$stackSettledHealthy = $false
for ($attempt = 0; $attempt -lt 10; $attempt++) {
    Start-Sleep -Seconds 1
    $stackText = Get-LiveStackText -InstanceId $instanceId
    $stackNames = Get-StackDriverNames -StackText $stackText
    if ($null -eq $stackNames) {
        $stackNames = @()
    }

    $deviceRebootRequired = Test-DeviceRebootRequired -InstanceId $instanceId
    $pendingFilterSummary = Get-PendingDeclarativeFilterSummary -InstanceId $instanceId
    if ((Test-SupportedHybridStack -StackNames $stackNames) -and -not $deviceRebootRequired) {
        $stackSettledHealthy = $true
        break
    }
}

$rebootRequired = $deviceRebootRequired -or ($reportedRebootRequired -and -not $stackSettledHealthy)
$driverRecordsAfter = Get-GaYmDriverRecords

Write-Host ''
Write-Host 'Install requested for the supported hybrid stack.'
Write-Host ("Artifact mode: {0}" -f $layout.Mode)
Write-Host "Target instance: $instanceId"
if ($stackNames.Count -gt 0) {
    Write-Host ("Normalized stack: {0}" -f ($stackNames -join ' -> '))
} else {
    Write-Warning 'Normalized stack is currently unavailable. The device may be in a reboot-required or problem state.'
}
if ($driverRecordsAfter.Count -gt 0) {
    Write-Host ("GaYm packages: {0}" -f (($driverRecordsAfter | ForEach-Object { "$($_.OriginalName)=$($_.PublishedName)" }) -join ', '))
}
if ($pendingFilterSummary) {
    Write-Host ("Pending declarative filters: {0}" -f $pendingFilterSummary)
}
if ($reportedRebootRequired -and $stackSettledHealthy -and -not $deviceRebootRequired) {
    Write-Host 'PnP requested a reboot during package install, but the live hybrid stack finished activating on this boot.'
}
Write-Host ''
Write-Host 'Live stack snapshot:'
Write-Host $stackText
Write-Host ''

if (-not (Test-SupportedHybridStack -StackNames $stackNames)) {
    Write-Warning 'The requested packages were installed, but the live hybrid stack is not fully visible yet. If Windows reports pending configuration, reboot before trusting runtime results.'
} else {
    Remove-InstallState -Path $installStatePath
    Write-Host 'The live stack already shows the supported hybrid path.'
}

if (-not $rebootRequired -and
    $stackAlreadySupported -and
    $lowerResult.UpToDate -and
    $upperResult.UpToDate -and
    $driverRecordsBefore.Count -eq $driverRecordsAfter.Count) {
    Write-Host 'Install is already current; no package changes were required.'
}

if ($rebootRequired) {
    Write-InstallState -Path $installStatePath -Configuration $Configuration -InstanceId $instanceId -BootTimeUtc $currentBootTimeUtc
    Write-Warning 'Windows reported that a reboot is required to finish applying one or both driver packages, or the device still reports a reboot-required state.'
    Write-Warning ("Install state saved to {0}. Reboot, then rerun smoke-test.ps1 or release-check.ps1." -f $installStatePath)
    if ($FailOnRebootRequired) {
        throw 'Installation reached a reboot-required state. Reboot before running runtime verification.'
    }
} elseif (-not (Test-SupportedHybridStack -StackNames $stackNames)) {
    Write-Warning 'The driver packages are present, but the active hybrid stack is not visible yet. Do not treat runtime verification failures on this boot as product failures until the reboot requirement has been cleared.'
} else {
    Remove-InstallState -Path $installStatePath
}
