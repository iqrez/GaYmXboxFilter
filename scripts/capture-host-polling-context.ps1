[CmdletBinding()]
param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Ensure-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    $repoRoot = Get-RepoRoot
    $defaultDir = Join-Path $repoRoot 'out\dev'
    New-Item -ItemType Directory -Force -Path $defaultDir | Out-Null
    return (Join-Path $defaultDir 'polling-host-context.txt')
}

function Get-DeviceInstanceByPattern {
    param([string]$Pattern)

    $device = Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like $Pattern } |
        Sort-Object Status, InstanceId |
        Select-Object -First 1

    if (-not $device) {
        return $null
    }

    return $device.InstanceId
}

function Get-DevicePropertyValue {
    param(
        [string]$InstanceId,
        [string]$KeyName
    )

    try {
        $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $KeyName -ErrorAction Stop
        return $property.Data
    } catch {
        return $null
    }
}

function Get-DeviceSummary {
    param([string]$InstanceId)

    $device = Get-PnpDevice -InstanceId $InstanceId -ErrorAction SilentlyContinue
    if (-not $device) {
        return $null
    }

    [pscustomobject]@{
        InstanceId   = $InstanceId
        FriendlyName = $device.FriendlyName
        Class        = $device.Class
        Status       = $device.Status
        Problem      = $device.Problem
        Parent       = Get-DevicePropertyValue -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Parent'
        Service      = Get-DevicePropertyValue -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Service'
        Driver       = Get-DevicePropertyValue -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Driver'
        Enumerator   = Get-DevicePropertyValue -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_EnumeratorName'
    }
}

function Get-ParentChain {
    param([string]$StartInstanceId)

    $chain = New-Object System.Collections.Generic.List[object]
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $current = $StartInstanceId

    while ($current -and $seen.Add($current)) {
        $summary = Get-DeviceSummary -InstanceId $current
        if (-not $summary) {
            break
        }

        $chain.Add($summary) | Out-Null
        $current = $summary.Parent
    }

    return $chain
}

function Get-ServiceImageInfo {
    param([string]$ServiceName)

    if ([string]::IsNullOrWhiteSpace($ServiceName)) {
        return $null
    }

    $serviceKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
    if (-not (Test-Path $serviceKey)) {
        return [pscustomobject]@{
            ServiceName = $ServiceName
            ImagePath   = $null
            ResolvedPath = $null
            FileVersion = $null
            ProductVersion = $null
        }
    }

    $imagePath = (Get-ItemProperty -Path $serviceKey -ErrorAction SilentlyContinue).ImagePath
    $resolvedPath = $null
    $fileVersion = $null
    $productVersion = $null

    if ($imagePath) {
        $resolvedPath = $imagePath.Trim('"')
        $resolvedPath = $resolvedPath -replace '^\\SystemRoot', $env:SystemRoot
        $resolvedPath = $resolvedPath -replace '^System32\\drivers', (Join-Path $env:SystemRoot 'System32\drivers')
        $resolvedPath = $resolvedPath -replace '^system32\\drivers', (Join-Path $env:SystemRoot 'System32\drivers')
        $resolvedPath = [Environment]::ExpandEnvironmentVariables($resolvedPath)

        if (Test-Path $resolvedPath) {
            $versionInfo = (Get-Item $resolvedPath).VersionInfo
            $fileVersion = $versionInfo.FileVersion
            $productVersion = $versionInfo.ProductVersion
        }
    }

    return [pscustomobject]@{
        ServiceName    = $ServiceName
        ImagePath      = $imagePath
        ResolvedPath   = $resolvedPath
        FileVersion    = $fileVersion
        ProductVersion = $productVersion
    }
}

function Add-Section {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Title,
        [string[]]$Body
    )

    $Lines.Add("## $Title") | Out-Null
    foreach ($line in $Body) {
        $Lines.Add($line) | Out-Null
    }
    $Lines.Add('') | Out-Null
}

$resolvedOutputPath = Ensure-OutputPath -RequestedPath $OutputPath
$outputDir = Split-Path -Parent $resolvedOutputPath
if ($outputDir) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

$parentInstance = Get-DeviceInstanceByPattern -Pattern 'USB\VID_045E&PID_0B12*'
$usbChildInstance = Get-DeviceInstanceByPattern -Pattern 'USB\VID_045E&PID_02FF&IG_00*'
$hidChildInstance = Get-DeviceInstanceByPattern -Pattern 'HID\VID_045E&PID_02FF&IG_00*'

$lines = New-Object System.Collections.Generic.List[string]
$timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'
$parentDisplay = if ($parentInstance) { $parentInstance } else { '<not present>' }
$usbChildDisplay = if ($usbChildInstance) { $usbChildInstance } else { '<not present>' }
$hidChildDisplay = if ($hidChildInstance) { $hidChildInstance } else { '<not present>' }
$lines.Add("# GaYm Host Polling Context Capture") | Out-Null
$lines.Add("Captured: $timestamp") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Target Nodes' -Body @(
    "Composite parent : $parentDisplay",
    "USB child        : $usbChildDisplay",
    "HID child        : $hidChildDisplay"
)

foreach ($instanceId in @($parentInstance, $usbChildInstance, $hidChildInstance)) {
    if (-not $instanceId) {
        continue
    }

    $summary = Get-DeviceSummary -InstanceId $instanceId
    $stackText = (& pnputil /enum-devices /instanceid $instanceId /stack /drivers | Out-String).TrimEnd()
    $rebootRequired = Get-DevicePropertyValue -InstanceId $instanceId -KeyName 'DEVPKEY_Device_IsRebootRequired'

    Add-Section -Lines $lines -Title "Node $instanceId" -Body @(
        "FriendlyName : $($summary.FriendlyName)",
        "Class        : $($summary.Class)",
        "Status       : $($summary.Status)",
        "Problem      : $($summary.Problem)",
        "Enumerator   : $($summary.Enumerator)",
        "Service      : $($summary.Service)",
        "Driver       : $($summary.Driver)",
        "Parent       : $($summary.Parent)",
        "RebootReq    : $rebootRequired",
        '',
        $stackText
    )
}

if ($parentInstance) {
    $chain = Get-ParentChain -StartInstanceId $parentInstance
    Add-Section -Lines $lines -Title 'Composite Parent Chain' -Body @(
        ($chain | ForEach-Object {
            "InstanceId=$($_.InstanceId) | Class=$($_.Class) | Status=$($_.Status) | Service=$($_.Service) | Driver=$($_.Driver)"
        })
    )

    $serviceNames = $chain |
        ForEach-Object { $_.Service } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -Unique

    $serviceInfoLines = foreach ($serviceName in $serviceNames) {
        $info = Get-ServiceImageInfo -ServiceName $serviceName
        "Service=$($info.ServiceName) | ImagePath=$($info.ImagePath) | ResolvedPath=$($info.ResolvedPath) | FileVersion=$($info.FileVersion) | ProductVersion=$($info.ProductVersion)"
    }

    Add-Section -Lines $lines -Title 'Parent Chain Service Images' -Body $serviceInfoLines
}

$knownDrivers = @(
    (Join-Path $env:SystemRoot 'System32\drivers\usbxhci.sys'),
    (Join-Path $env:SystemRoot 'System32\drivers\usbport.sys'),
    (Join-Path $env:SystemRoot 'System32\drivers\USBHUB3.SYS'),
    (Join-Path $env:SystemRoot 'System32\drivers\hidusb.sys'),
    (Join-Path $env:SystemRoot 'System32\drivers\xboxgip.sys'),
    (Join-Path $env:SystemRoot 'System32\drivers\xinputhid.sys')
)

$driverVersionLines = foreach ($driverPath in $knownDrivers) {
    if (-not (Test-Path $driverPath)) {
        "Path=$driverPath | Present=False"
        continue
    }

    $versionInfo = (Get-Item $driverPath).VersionInfo
    "Path=$driverPath | Present=True | FileVersion=$($versionInfo.FileVersion) | ProductVersion=$($versionInfo.ProductVersion)"
}

Add-Section -Lines $lines -Title 'Known Driver Versions' -Body $driverVersionLines

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "Host polling context written to $resolvedOutputPath"
