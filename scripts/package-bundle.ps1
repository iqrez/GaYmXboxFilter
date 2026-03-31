[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$BundleName,

    [string]$OutputRoot,

    [switch]$SkipIfInputsMissing
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$profileSegment = if ($Configuration -eq 'Debug') { 'dev' } else { 'release' }
$stageRoot = Join-Path $repoRoot ("out\" + $profileSegment)

if (-not $BundleName) {
    $BundleName = "GaYmXboxFilter-authoritative-$($profileSegment)-bundle-$timestamp"
}
if (-not $OutputRoot) {
    $OutputRoot = Join-Path $repoRoot 'out\bundles'
}

$bundleRoot = Join-Path $OutputRoot $BundleName
$zipPath = Join-Path $OutputRoot ($BundleName + '.zip')
$bundleStageRoot = Join-Path $bundleRoot ("out\" + $profileSegment)

$requiredPaths = @(
    (Join-Path $stageRoot 'client\gaym_client.lib'),
    (Join-Path $stageRoot 'driver\GaYmFilter.inf'),
    (Join-Path $stageRoot 'driver\GaYmFilter.sys'),
    (Join-Path $stageRoot 'driver\gaymfilter.cat'),
    (Join-Path $stageRoot 'driver\GaYmFilter.cer'),
    (Join-Path $stageRoot 'upper\GaYmXInputFilter.inf'),
    (Join-Path $stageRoot 'upper\GaYmXInputFilter.sys'),
    (Join-Path $stageRoot 'upper\gaymxinputfilter.cat'),
    (Join-Path $stageRoot 'upper\GaYmXInputFilter.cer'),
    (Join-Path $stageRoot 'tools\GaYmCLI.exe'),
    (Join-Path $stageRoot 'tools\GaYmFeeder.exe'),
    (Join-Path $stageRoot 'tools\MinimalTestFeeder.exe'),
    (Join-Path $stageRoot 'tools\XInputCheck.exe'),
    (Join-Path $stageRoot 'tools\AutoVerify.exe'),
    (Join-Path $repoRoot 'scripts\install-driver.ps1'),
    (Join-Path $repoRoot 'scripts\uninstall-driver.ps1'),
    (Join-Path $repoRoot 'scripts\smoke-test.ps1'),
    (Join-Path $repoRoot 'README.md'),
    (Join-Path $repoRoot 'PROJECT_STATE.md'),
    (Join-Path $repoRoot 'CONTRACTOR_HANDBOOK.md')
)

$missingRequiredPaths = @()
foreach ($requiredPath in $requiredPaths) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        $missingRequiredPaths += $requiredPath
    }
}

if ($missingRequiredPaths.Count -ne 0) {
    if ($SkipIfInputsMissing) {
        Write-Host "Skipping bundle refresh because staged inputs are incomplete:"
        $missingRequiredPaths | ForEach-Object { Write-Host "  $_" }
        return
    }

    throw "Required bundle input not found: $($missingRequiredPaths[0])"
}

function Copy-IfPresent {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,
        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    if (Test-Path -LiteralPath $Source) {
        $destinationDirectory = Split-Path -Parent $Destination
        if (-not (Test-Path -LiteralPath $destinationDirectory)) {
            New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
        }
        Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop
    }
}

function Remove-PathWithRetry {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [int]$Attempts = 5,
        [int]$DelayMilliseconds = 500
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            return
        } catch {
            if ($attempt -eq $Attempts) {
                throw
            }

            Start-Sleep -Milliseconds $DelayMilliseconds
        }
    }
}

if (Test-Path -LiteralPath $bundleRoot) {
    Remove-PathWithRetry -Path $bundleRoot
}
if (Test-Path -LiteralPath $zipPath) {
    try {
        Remove-Item -LiteralPath $zipPath -Force -ErrorAction Stop
    } catch {
        Write-Warning "Could not replace bundle zip because it is locked: $zipPath"
        $zipPath = $null
    }
}

New-Item -ItemType Directory -Force -Path $bundleRoot | Out-Null
New-Item -ItemType Directory -Force -Path $bundleStageRoot | Out-Null

foreach ($directoryName in @('client', 'driver', 'upper', 'tools', 'verification')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $bundleStageRoot $directoryName) | Out-Null
}
foreach ($directoryName in @('scripts', 'docs', 'include\client', 'include\shared')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $bundleRoot $directoryName) | Out-Null
}

Copy-IfPresent -Source (Join-Path $stageRoot 'client\gaym_client.lib') -Destination (Join-Path $bundleStageRoot 'client\gaym_client.lib')

foreach ($driverFile in @('GaYmFilter.inf', 'GaYmFilter.sys', 'GaYmFilter.cat', 'gaymfilter.cat', 'GaYmFilter.cer')) {
    Copy-IfPresent -Source (Join-Path $stageRoot "driver\$driverFile") -Destination (Join-Path $bundleStageRoot "driver\$driverFile")
}
foreach ($upperFile in @('GaYmXInputFilter.inf', 'GaYmXInputFilter.sys', 'GaYmXInputFilter.cat', 'gaymxinputfilter.cat', 'GaYmXInputFilter.cer')) {
    Copy-IfPresent -Source (Join-Path $stageRoot "upper\$upperFile") -Destination (Join-Path $bundleStageRoot "upper\$upperFile")
}
foreach ($toolFile in @('GaYmCLI.exe', 'GaYmFeeder.exe', 'MinimalTestFeeder.exe', 'XInputCheck.exe', 'AutoVerify.exe')) {
    Copy-IfPresent -Source (Join-Path $stageRoot "tools\$toolFile") -Destination (Join-Path $bundleStageRoot "tools\$toolFile")
}
Copy-IfPresent -Source (Join-Path $stageRoot 'verification\autoverify-state.json') -Destination (Join-Path $bundleStageRoot 'verification\autoverify-state.json')

foreach ($scriptName in @('install-driver.ps1', 'uninstall-driver.ps1', 'smoke-test.ps1', 'build-driver.ps1', 'build-tools.ps1', 'build-client.ps1', 'sign-driver.ps1', 'sign-driver-packages.ps1', 'README.md')) {
    Copy-IfPresent -Source (Join-Path $repoRoot "scripts\$scriptName") -Destination (Join-Path $bundleRoot "scripts\$scriptName")
}
foreach ($docName in @('ARCHITECTURE.md', 'ARCHITECTURE_DECISIONS.md', 'diagnostics.md', 'MAINTAINER_TOOLS.md')) {
    Copy-IfPresent -Source (Join-Path $repoRoot "docs\$docName") -Destination (Join-Path $bundleRoot "docs\$docName")
}
foreach ($rootDoc in @('README.md', 'PROJECT_STATE.md', 'CONTRACTOR_HANDBOOK.md')) {
    Copy-IfPresent -Source (Join-Path $repoRoot $rootDoc) -Destination (Join-Path $bundleRoot $rootDoc)
}
foreach ($header in @('gaym_client.h')) {
    Copy-IfPresent -Source (Join-Path $repoRoot "src\client\$header") -Destination (Join-Path $bundleRoot "include\client\$header")
}
foreach ($header in @('protocol.h', 'ioctl.h', 'gaym_report.h', 'gaym_observation.h', 'device_ids.h', 'capability_flags.h')) {
    Copy-IfPresent -Source (Join-Path $repoRoot "src\shared\$header") -Destination (Join-Path $bundleRoot "include\shared\$header")
}

$manifestLines = @(
    "BundleName: $BundleName",
    "Configuration: $Configuration",
    "CreatedUtc: $([DateTime]::UtcNow.ToString('o'))",
    "ProfileRoot: out/$profileSegment",
    "SupportedHardware: HID\VID_045E&PID_02FF&IG_00",
    "SupportedStack: GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb",
    "IncludedTools: GaYmCLI.exe, GaYmFeeder.exe, MinimalTestFeeder.exe, XInputCheck.exe, AutoVerify.exe",
    "DiagnosticOnlyNotStaged: QuickVerify.exe, QuickVerifyHid.exe, XInputAutoVerify.exe",
    "AutoVerifyStatePresent: $((Test-Path -LiteralPath (Join-Path $stageRoot 'verification\autoverify-state.json')).ToString().ToUpperInvariant())",
    "LowerPackageCatPresent: $((Test-Path -LiteralPath (Join-Path $stageRoot 'driver\gaymfilter.cat')).ToString().ToUpperInvariant())",
    "UpperPackageCatPresent: $((Test-Path -LiteralPath (Join-Path $stageRoot 'upper\gaymxinputfilter.cat')).ToString().ToUpperInvariant())"
)
$manifestLines | Set-Content -LiteralPath (Join-Path $bundleRoot 'BUNDLE-MANIFEST.txt') -Encoding ASCII

if (-not (Test-Path -LiteralPath $OutputRoot)) {
    New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
}

if ($zipPath) {
    try {
        Compress-Archive -Path (Join-Path $bundleRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal -ErrorAction Stop
    } catch {
        Write-Warning "Bundle root refreshed, but zip update failed: $($_.Exception.Message)"
        $zipPath = $null
    }
}

Write-Host "Bundle root: $bundleRoot"
if ($zipPath) {
    Write-Host "Bundle zip: $zipPath"
} else {
    Write-Host "Bundle zip: skipped"
}
Get-ChildItem -LiteralPath $bundleRoot -Recurse -File | Select-Object FullName, Length
