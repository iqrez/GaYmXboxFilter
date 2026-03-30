[CmdletBinding()]
param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    $defaultDir = Join-Path (Get-RepoRoot) 'out\dev'
    New-Item -ItemType Directory -Force -Path $defaultDir | Out-Null
    return (Join-Path $defaultDir 'usbxhci-recon.txt')
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

function Get-DevicePropertyValue {
    param(
        [string]$InstanceId,
        [string]$KeyName
    )

    try {
        return (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $KeyName -ErrorAction Stop).Data
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

    return [pscustomobject]@{
        InstanceId    = $device.InstanceId
        FriendlyName  = $device.FriendlyName
        Class         = $device.Class
        Status        = $device.Status
        Problem       = $device.Problem
        Service       = Get-DevicePropertyValue -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Service'
        Driver        = Get-DevicePropertyValue -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
        Parent        = Get-DevicePropertyValue -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Parent'
        LocationPaths = Get-DevicePropertyValue -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_LocationPaths'
        Children      = Get-DevicePropertyValue -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Children'
    }
}

function Resolve-ServiceImagePath {
    param([string]$ImagePath)

    if ([string]::IsNullOrWhiteSpace($ImagePath)) {
        return $null
    }

    $resolvedPath = $ImagePath.Trim('"')
    $resolvedPath = $resolvedPath -replace '^\\SystemRoot', $env:SystemRoot
    $resolvedPath = $resolvedPath -replace '^System32\\drivers', (Join-Path $env:SystemRoot 'System32\drivers')
    $resolvedPath = $resolvedPath -replace '^system32\\drivers', (Join-Path $env:SystemRoot 'System32\drivers')
    return [Environment]::ExpandEnvironmentVariables($resolvedPath)
}

function Get-ServiceSnapshot {
    param([string]$ServiceName)

    $serviceKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
    if (-not (Test-Path $serviceKey)) {
        return [pscustomobject]@{
            ServiceName      = $ServiceName
            Present          = $false
            ImagePath        = $null
            ResolvedPath     = $null
            Start            = $null
            Type             = $null
            ErrorControl     = $null
            DependOnService  = @()
            Owners           = @()
            FileVersion      = $null
            ProductVersion   = $null
            FileHashSha256   = $null
            SignatureStatus  = $null
            SignatureSigner  = $null
        }
    }

    $props = Get-ItemProperty -Path $serviceKey
    $resolvedPath = Resolve-ServiceImagePath -ImagePath $props.ImagePath
    $fileVersion = $null
    $productVersion = $null
    $fileHash = $null
    $signatureStatus = $null
    $signatureSigner = $null

    if ($resolvedPath -and (Test-Path $resolvedPath)) {
        $versionInfo = (Get-Item $resolvedPath).VersionInfo
        $fileVersion = $versionInfo.FileVersion
        $productVersion = $versionInfo.ProductVersion
        $fileHash = (Get-FileHash -Path $resolvedPath -Algorithm SHA256).Hash
        $signature = Get-AuthenticodeSignature -FilePath $resolvedPath
        $signatureStatus = $signature.Status
        if ($signature.SignerCertificate) {
            $signatureSigner = $signature.SignerCertificate.Subject
        }
    }

    return [pscustomobject]@{
        ServiceName      = $ServiceName
        Present          = $true
        ImagePath        = $props.ImagePath
        ResolvedPath     = $resolvedPath
        Start            = $props.Start
        Type             = $props.Type
        ErrorControl     = $props.ErrorControl
        DependOnService  = @($props.DependOnService)
        Owners           = @($props.Owners)
        FileVersion      = $fileVersion
        ProductVersion   = $productVersion
        FileHashSha256   = $fileHash
        SignatureStatus  = $signatureStatus
        SignatureSigner  = $signatureSigner
    }
}

function Resolve-ChildDeviceSummaries {
    param([object[]]$ChildInstanceIds)

    $result = New-Object System.Collections.Generic.List[object]
    foreach ($childInstanceId in @($ChildInstanceIds)) {
        if ([string]::IsNullOrWhiteSpace($childInstanceId)) {
            continue
        }

        $summary = Get-DeviceSummary -InstanceId $childInstanceId
        if ($summary) {
            $result.Add($summary) | Out-Null
        }
    }

    return ($result | Sort-Object Class, FriendlyName, InstanceId)
}

$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath
$outputDir = Split-Path -Parent $resolvedOutputPath
if ($outputDir) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

$lines = New-Object System.Collections.Generic.List[string]
$timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'
$usbxhciControllers = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.Service -eq 'USBXHCI' } |
    Sort-Object FriendlyName, InstanceId
$targetParentInstance = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'USB\VID_045E&PID_0B12*' } |
    Select-Object -First 1 -ExpandProperty InstanceId

$lines.Add('# USBXHCI Recon Capture') | Out-Null
$lines.Add("Captured: $timestamp") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "USBXHCI controllers detected : $($usbxhciControllers.Count)",
    "Target composite parent      : $(if ($targetParentInstance) { $targetParentInstance } else { '<not present>' })"
)

$serviceNames = @('USBXHCI', 'USBHUB3', 'Ucx01000', 'dc1-controller')
foreach ($serviceName in $serviceNames) {
    $snapshot = Get-ServiceSnapshot -ServiceName $serviceName
    Add-Section -Lines $lines -Title "Service $serviceName" -Body @(
        "Present         : $($snapshot.Present)",
        "ImagePath       : $($snapshot.ImagePath)",
        "ResolvedPath    : $($snapshot.ResolvedPath)",
        "Start           : $($snapshot.Start)",
        "Type            : $($snapshot.Type)",
        "ErrorControl    : $($snapshot.ErrorControl)",
        "DependOnService : $([string]::Join(', ', $snapshot.DependOnService))",
        "Owners          : $([string]::Join(', ', $snapshot.Owners))",
        "FileVersion     : $($snapshot.FileVersion)",
        "ProductVersion  : $($snapshot.ProductVersion)",
        "SHA256          : $($snapshot.FileHashSha256)",
        "SigStatus       : $($snapshot.SignatureStatus)",
        "SigSigner       : $($snapshot.SignatureSigner)"
    )
}

foreach ($controller in $usbxhciControllers) {
    $summary = Get-DeviceSummary -InstanceId $controller.InstanceId
    $childInstanceIds = @($summary.Children | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $children = Resolve-ChildDeviceSummaries -ChildInstanceIds $childInstanceIds
    $stackText = (& pnputil /enum-devices /instanceid $controller.InstanceId /stack /drivers | Out-String).TrimEnd()

    Add-Section -Lines $lines -Title "USBXHCI Controller $($controller.InstanceId)" -Body @(
        "FriendlyName  : $($summary.FriendlyName)",
        "Status        : $($summary.Status)",
        "Problem       : $($summary.Problem)",
        "Service       : $($summary.Service)",
        "Driver        : $($summary.Driver)",
        "Parent        : $($summary.Parent)",
        "LocationPaths : $([string]::Join(', ', @($summary.LocationPaths)))",
        "ChildrenCount : $($childInstanceIds.Count)",
        '',
        $stackText
    )

    if ($children.Count -gt 0) {
        Add-Section -Lines $lines -Title "USBXHCI Children $($controller.InstanceId)" -Body @(
            ($children | ForEach-Object {
                "InstanceId=$($_.InstanceId) | Class=$($_.Class) | FriendlyName=$($_.FriendlyName) | Service=$($_.Service) | Driver=$($_.Driver)"
            })
        )
    }
}

if ($targetParentInstance) {
    $targetChain = New-Object System.Collections.Generic.List[string]
    $currentInstance = $targetParentInstance
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

    while ($currentInstance -and $seen.Add($currentInstance)) {
        $summary = Get-DeviceSummary -InstanceId $currentInstance
        if (-not $summary) {
            break
        }

        $targetChain.Add(
            "InstanceId=$($summary.InstanceId) | Class=$($summary.Class) | FriendlyName=$($summary.FriendlyName) | Service=$($summary.Service) | Driver=$($summary.Driver)"
        ) | Out-Null

        $currentInstance = $summary.Parent
    }

    Add-Section -Lines $lines -Title 'Target Composite Ancestry' -Body $targetChain
}

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "USBXHCI recon written to $resolvedOutputPath"
