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
    return (Join-Path $defaultDir 'usbxhci-patchability.txt')
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

function Get-PresentUsbXhciControllers {
    return Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.Service -eq 'USBXHCI' } |
        Sort-Object FriendlyName, InstanceId
}

function Get-TargetControllerFromRecon {
    $reconPath = Join-Path (Join-Path (Get-RepoRoot) 'out\dev') 'usbxhci-recon.txt'
    if (-not (Test-Path $reconPath)) {
        return $null
    }

    $content = Get-Content -Raw $reconPath
    $match = [regex]::Match(
        $content,
        'InstanceId=(?<instance>PCI\\VEN_[^|]+)\s+\|\s+Class=USB\s+\|\s+FriendlyName=(?<name>[^|]+)\s+\|\s+Service=USBXHCI',
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

    if (-not $match.Success) {
        return $null
    }

    return [pscustomobject]@{
        InstanceId   = $match.Groups['instance'].Value.Trim()
        FriendlyName = $match.Groups['name'].Value.Trim()
        Service      = 'USBXHCI'
    }
}

function Resolve-TargetControllerDetails {
    param(
        [psobject]$TargetController,
        [object[]]$Controllers
    )

    if (-not $TargetController) {
        return $null
    }

    $presentMatch = $Controllers |
        Where-Object { $_.InstanceId -eq $TargetController.InstanceId } |
        Select-Object -First 1

    return [pscustomobject]@{
        FriendlyName = if ($presentMatch -and $presentMatch.FriendlyName) { $presentMatch.FriendlyName } else { $TargetController.FriendlyName }
        Status       = if ($presentMatch -and $presentMatch.Status) { $presentMatch.Status } else { '<unknown>' }
        Class        = if ($presentMatch -and $presentMatch.Class) { $presentMatch.Class } else { 'USB' }
        Service      = if ($presentMatch -and $presentMatch.Service) { $presentMatch.Service } else { $TargetController.Service }
        InstanceId   = $TargetController.InstanceId
    }
}

function Get-UsbXhciVersionInfo {
    $path = Join-Path $env:SystemRoot 'System32\drivers\USBXHCI.SYS'
    if (-not (Test-Path $path)) {
        return $null
    }

    $file = Get-Item $path
    $signature = Get-AuthenticodeSignature -FilePath $path

    return [pscustomobject]@{
        Path           = $path
        FileVersion    = $file.VersionInfo.FileVersion
        ProductVersion = $file.VersionInfo.ProductVersion
        SHA256         = (Get-FileHash -Path $path -Algorithm SHA256).Hash
        Signature      = $signature.Status
        Signer         = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { $null }
    }
}

function Get-DocumentedHidUsbFnRange {
    $readmePath = 'C:\Users\IQRez\Downloads\hidusbf-master\hidusbf-master\hidusbfn\readme.eng.txt'
    if (-not (Test-Path $readmePath)) {
        return $null
    }

    $content = Get-Content -Raw $readmePath
    $match = [regex]::Match(
        $content,
        '11 21H2\+\s+usbxhci\.sys\s+Yes\s+LS,FS->HS\s+\(10\.0\.(?<minBuild>\d+)\.(?<minRev>\d+)-\s*10\.0\.(?<maxBuild>\d+)\.(?<maxRev>\d+)\)',
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
        [System.Text.RegularExpressions.RegexOptions]::Singleline)

    if (-not $match.Success) {
        return $null
    }

    $minVersion = [version]::new(10, 0, [int]$match.Groups['minBuild'].Value, [int]$match.Groups['minRev'].Value)
    $maxVersion = [version]::new(10, 0, [int]$match.Groups['maxBuild'].Value, [int]$match.Groups['maxRev'].Value)

    return [pscustomobject]@{
        SourcePath = $readmePath
        Minimum    = $minVersion
        Maximum    = $maxVersion
    }
}

function Parse-VersionPrefix {
    param([string]$VersionText)

    if ([string]::IsNullOrWhiteSpace($VersionText)) {
        return $null
    }

    $match = [regex]::Match($VersionText, '^\d+\.\d+\.\d+\.\d+')
    if (-not $match.Success) {
        return $null
    }

    return [version]$match.Value
}

$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath
$outputDir = Split-Path -Parent $resolvedOutputPath
if ($outputDir) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

$controllers = @(Get-PresentUsbXhciControllers)
$targetController = Resolve-TargetControllerDetails -TargetController (Get-TargetControllerFromRecon) -Controllers $controllers
$versionInfo = Get-UsbXhciVersionInfo
$documentedRange = Get-DocumentedHidUsbFnRange
$activeVersion = if ($versionInfo) { Parse-VersionPrefix -VersionText $versionInfo.ProductVersion } else { $null }

$inDocumentedRange = $false
if ($activeVersion -and $documentedRange) {
    $inDocumentedRange = ($activeVersion -ge $documentedRange.Minimum -and $activeVersion -le $documentedRange.Maximum)
}

$assessment = if (-not $versionInfo) {
    'No active USBXHCI image found; host-stack patchability cannot be assessed.'
} elseif (-not $documentedRange) {
    'USBXHCI image identified, but the local hidusbfn documentation range could not be parsed. Patchability remains unknown.'
} elseif ($inDocumentedRange) {
    'Active USBXHCI image falls within the documented hidusbfn Win11 USBXHCI range. A host-stack experiment is at least version-plausible.'
} elseif ($activeVersion -gt $documentedRange.Maximum) {
    'Active USBXHCI image is newer than the documented hidusbfn Win11 USBXHCI range. Direct reuse of hidusbf-era patch assumptions is not validated on this box.'
} else {
    'Active USBXHCI image is older than the documented hidusbfn Win11 USBXHCI range. Compatibility is not validated on this box.'
}

$riskSummary = if ($targetController) {
    @(
        'Any host-stack experiment would target the controller serving the live Xbox path, not an isolated virtual test layer.',
        'Blast radius includes every device behind that xHCI controller and its root hub.',
        'The current supported hybrid product line does not depend on host-stack patching.'
    )
} else {
    @('Target Xbox controller ancestry could not be resolved to a USBXHCI controller in this session.')
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Patchability Assessment') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "USBXHCI controller count : $($controllers.Count)",
    "Target controller        : $(if ($targetController) { $targetController.InstanceId } else { '<not resolved>' })",
    "Assessment               : $assessment"
)

if ($versionInfo) {
    Add-Section -Lines $lines -Title 'Active USBXHCI Image' -Body @(
        "Path           : $($versionInfo.Path)",
        "FileVersion    : $($versionInfo.FileVersion)",
        "ProductVersion : $($versionInfo.ProductVersion)",
        "SHA256         : $($versionInfo.SHA256)",
        "Signature      : $($versionInfo.Signature)",
        "Signer         : $($versionInfo.Signer)"
    )
}

if ($documentedRange) {
    Add-Section -Lines $lines -Title 'Documented hidusbfn Win11 USBXHCI Range' -Body @(
        "Source  : $($documentedRange.SourcePath)",
        "Minimum : $($documentedRange.Minimum)",
        "Maximum : $($documentedRange.Maximum)",
        "Matches : $inDocumentedRange"
    )
}

if ($targetController) {
    Add-Section -Lines $lines -Title 'Target Controller' -Body @(
        "FriendlyName : $($targetController.FriendlyName)",
        "Status       : $($targetController.Status)",
        "Class        : $($targetController.Class)",
        "Service      : $($targetController.Service)",
        "InstanceId   : $($targetController.InstanceId)"
    )
}

Add-Section -Lines $lines -Title 'Risk Notes' -Body $riskSummary

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "USBXHCI patchability assessment written to $resolvedOutputPath"
