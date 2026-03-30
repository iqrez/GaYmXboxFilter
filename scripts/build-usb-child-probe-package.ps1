[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$inf2Cat = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter Inf2Cat.exe |
    Select-Object -First 1 -ExpandProperty FullName
$signTool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
    Where-Object { $_.FullName -like '*\x64\signtool.exe' } |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $inf2Cat) {
    throw 'Inf2Cat.exe was not found.'
}
if (-not $signTool) {
    throw 'signtool.exe was not found.'
}

$stageRoot = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev'
} else {
    Join-Path $repoRoot 'out\release'
}

$baseLowerPackage = Join-Path $stageRoot 'driver-lower\package'
$baseCatalog = Join-Path $baseLowerPackage 'gaymfilter.cat'
$baseBinary = Join-Path $baseLowerPackage 'GaYmFilter.sys'
$baseCert = Join-Path $baseLowerPackage 'GaYmFilter.cer'
$probeStageRoot = Join-Path $stageRoot 'polling-usb-child-probe'
$probePackageRoot = Join-Path $probeStageRoot 'package'
$probeInfTemplate = Join-Path $repoRoot 'GaYmFilter\GaYmUsbChildProbe.inf'
$probeInf = Join-Path $probePackageRoot 'GaYmUsbChildProbe.inf'
$probeCat = Join-Path $probePackageRoot 'GaYmUsbChildProbe.cat'
$probeBinary = Join-Path $probePackageRoot 'GaYmFilter.sys'
$probeCert = Join-Path $probePackageRoot 'GaYmFilter.cer'

foreach ($requiredPath in @($baseLowerPackage, $baseCatalog, $baseBinary, $baseCert, $probeInfTemplate)) {
    if (-not (Test-Path $requiredPath)) {
        throw "Required file not found: $requiredPath. Run scripts\build-driver.ps1 first."
    }
}

$signature = Get-AuthenticodeSignature $baseCatalog
if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate) {
    throw "Base lower package catalog is not validly signed: $baseCatalog"
}

$thumbprint = $signature.SignerCertificate.Thumbprint
$signingCert = Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue |
    Where-Object { $_.Thumbprint -eq $thumbprint -and $_.HasPrivateKey } |
    Select-Object -First 1

if (-not $signingCert) {
    $signingCert = Get-ChildItem Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $thumbprint -and $_.HasPrivateKey } |
        Select-Object -First 1
}

if (-not $signingCert) {
    throw "Signing certificate $thumbprint with a private key was not found in CurrentUser\\My or LocalMachine\\My."
}

New-Item -ItemType Directory -Force -Path $probePackageRoot | Out-Null
Get-ChildItem $probePackageRoot -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $probePackageRoot | Out-Null

Copy-Item -Force $probeInfTemplate $probeInf
Copy-Item -Force $baseBinary $probeBinary
Copy-Item -Force $baseCert $probeCert

& $inf2Cat /driver:$probePackageRoot /os:10_NI_X64,10_GE_X64 /uselocaltime
if ($LASTEXITCODE -ne 0) {
    throw 'Inf2Cat failed for the USB-child probe package.'
}

$signArguments = @(
    'sign',
    '/fd', 'SHA256',
    '/sha1', $signingCert.Thumbprint,
    '/s', 'My'
)

if ($signingCert.PSParentPath -like '*LocalMachine*') {
    $signArguments += '/sm'
}

$signArguments += $probeCat

& $signTool @signArguments
if ($LASTEXITCODE -ne 0) {
    throw 'signtool failed for the USB-child probe package.'
}

$probeSignature = Get-AuthenticodeSignature $probeCat
if ($probeSignature.Status -ne 'Valid') {
    throw "Probe package catalog signature is not valid: $($probeSignature.StatusMessage)"
}

Write-Host "USB-child probe package: $probePackageRoot"
Write-Host "Catalog: $probeCat"
Write-Host "Signer thumbprint: $($signingCert.Thumbprint)"
