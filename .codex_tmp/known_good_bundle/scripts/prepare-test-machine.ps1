[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$EnableTestSigning
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common-paths.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $Configuration
$certificatePaths = Get-GaYmCertificatePaths -Layout $layout

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run prepare-test-machine.ps1 from an elevated PowerShell session.'
    }
}

function Import-CertificateIfMissing {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CertificatePath,
        [Parameter(Mandatory = $true)]
        [string]$StoreLocation
    )

    $certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($CertificatePath)
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($StoreLocation, 'LocalMachine')
    $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    try {
        $existing = $store.Certificates | Where-Object { $_.Thumbprint -eq $certificate.Thumbprint } | Select-Object -First 1
        if ($existing) {
            return [pscustomobject]@{
                Subject    = $certificate.Subject
                Thumbprint = $certificate.Thumbprint
                Store      = $StoreLocation
                Imported   = $false
            }
        }

        $store.Add($certificate)
        return [pscustomobject]@{
            Subject    = $certificate.Subject
            Thumbprint = $certificate.Thumbprint
            Store      = $StoreLocation
            Imported   = $true
        }
    } finally {
        $store.Close()
    }
}

function Get-UniqueCertificateFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Paths
    )

    $seenThumbprints = New-Object 'System.Collections.Generic.HashSet[string]'
    $uniquePaths = New-Object 'System.Collections.Generic.List[string]'
    foreach ($path in $Paths) {
        $certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($path)
        if ($seenThumbprints.Add($certificate.Thumbprint)) {
            $uniquePaths.Add($path)
        }
    }

    return $uniquePaths
}

function Test-TestSigningEnabled {
    $output = & bcdedit /enum '{current}' 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw 'bcdedit /enum "{current}" failed.'
    }

    return (($output -join [Environment]::NewLine) -match '(?im)^\s*testsigning\s+yes\s*$')
}

Assert-Administrator

if ($certificatePaths.Count -eq 0) {
    throw 'No GaYm test certificates were found in the current repo or extracted bundle layout.'
}

$certificatePaths = Get-UniqueCertificateFiles -Paths $certificatePaths

Write-Host ("Artifact mode: {0}" -f $layout.Mode)
Write-Host '=== Importing GaYm test certificates ==='

$imports = New-Object 'System.Collections.Generic.List[object]'
foreach ($certificatePath in $certificatePaths) {
    foreach ($storeName in @('Root', 'TrustedPublisher')) {
        $imports.Add((Import-CertificateIfMissing -CertificatePath $certificatePath -StoreLocation $storeName))
    }
}

$imports |
    Sort-Object Thumbprint, Store |
    ForEach-Object {
        $action = if ($_.Imported) { 'Imported' } else { 'Already trusted' }
        Write-Host ("{0}: {1} [{2}] in LocalMachine\\{3}" -f $action, $_.Subject, $_.Thumbprint, $_.Store)
    }

$testSigningEnabled = Test-TestSigningEnabled
Write-Host ''
Write-Host ("Current testsigning state: {0}" -f ($(if ($testSigningEnabled) { 'ENABLED' } else { 'DISABLED' })))

if (-not $testSigningEnabled -and $EnableTestSigning) {
    Write-Host ''
    Write-Host '=== Enabling testsigning ==='
    $output = & bcdedit /set testsigning on 2>&1
    $outputText = $output -join [Environment]::NewLine
    if ($outputText) {
        Write-Host $outputText
    }

    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to enable testsigning. Secure Boot or local policy may be blocking this change.'
    }

    Write-Warning 'Testsigning was enabled. Reboot before attempting driver install.'
    return
}

if (-not $testSigningEnabled) {
    Write-Warning 'Testsigning is currently disabled. Fresh test machines typically need testsigning enabled before these test-signed driver packages will install and load.'
    Write-Host 'Recommended next step:'
    Write-Host '  bcdedit /set testsigning on'
    Write-Host 'Then reboot and rerun install-driver.ps1.'
    Write-Warning 'If Secure Boot blocks test mode, disable Secure Boot for the test machine or use a production-signed package instead.'
} else {
    Write-Host 'The machine is prepared for test-signed driver installation.'
}
