[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$wdkBinRoot = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0'
$inf2cat = Join-Path $wdkBinRoot 'x86\Inf2Cat.exe'
$signtool = Join-Path $wdkBinRoot 'x64\signtool.exe'
$stageRoot = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev'
} else {
    Join-Path $repoRoot 'out\release'
}
$certificatePath = Join-Path $PSScriptRoot 'GaYmXboxFilter-TestCodeSigning.pfx'
$certificatePasswordText = 'GaYmXboxFilter-TestCert'
$certificatePassword = ConvertTo-SecureString -String $certificatePasswordText -AsPlainText -Force
$timestampUrl = 'http://timestamp.digicert.com'

if (-not (Test-Path -LiteralPath $inf2cat)) {
    throw "Inf2Cat.exe not found: $inf2cat"
}
if (-not (Test-Path -LiteralPath $signtool)) {
    throw "signtool.exe not found: $signtool"
}
if (-not (Test-Path -LiteralPath $certificatePath)) {
    throw "Test signing certificate not found: $certificatePath"
}
if (-not (Test-Path -LiteralPath $stageRoot)) {
    throw "Driver staging root not found: $stageRoot"
}

function Invoke-SignCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & $signtool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed: $($Arguments -join ' ')"
    }
}

function Resolve-SigningCertificate {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PfxPath,

        [Parameter(Mandatory = $true)]
        [SecureString]$Password
    )

    $passwordPtr = [System.Runtime.InteropServices.Marshal]::SecureStringToGlobalAllocUnicode($Password)
    try {
        $plainTextPassword = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($passwordPtr)
        $pfxCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
            $PfxPath,
            $plainTextPassword,
            [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable
        )
    } finally {
        if ($passwordPtr -ne [IntPtr]::Zero) {
            [System.Runtime.InteropServices.Marshal]::ZeroFreeGlobalAllocUnicode($passwordPtr)
        }
    }

    $thumbprint = $pfxCertificate.Thumbprint
    $existingCertificate = Get-ChildItem Cert:\CurrentUser\My,Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $thumbprint } |
        Select-Object -First 1

    if ($existingCertificate) {
        return $existingCertificate
    }

    $importedCertificate = Import-PfxCertificate -FilePath $PfxPath -CertStoreLocation Cert:\CurrentUser\My -Password $Password -Exportable
    if (-not $importedCertificate) {
        throw "Unable to import signing certificate from: $PfxPath"
    }

    return $importedCertificate | Select-Object -First 1
}

function Resolve-DriverPackageRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackageRoot,

        [Parameter(Mandatory = $true)]
        [string]$InfName,

        [Parameter(Mandatory = $true)]
        [string]$SysName,

        [Parameter(Mandatory = $true)]
        [string]$CerName
    )

    $candidateRoots = @(
        $PackageRoot,
        (Join-Path $PackageRoot 'package')
    )

    foreach ($candidateRoot in $candidateRoots) {
        $infPath = Join-Path $candidateRoot $InfName
        $sysPath = Join-Path $candidateRoot $SysName

        $hasInf = Test-Path -LiteralPath $infPath
        $hasSys = Test-Path -LiteralPath $sysPath
        if ($hasInf -and $hasSys) {
            return [pscustomobject]@{
                SourceRoot       = $candidateRoot
                MirrorRoot       = if ($candidateRoot -ieq $PackageRoot) { Join-Path $PackageRoot 'package' } else { $PackageRoot }
                InfPath          = $infPath
                SysPath          = $sysPath
                CatPath          = Join-Path $candidateRoot ([System.IO.Path]::ChangeExtension($InfName, '.cat'))
                LowerCatPath     = Join-Path $candidateRoot ([System.IO.Path]::GetFileNameWithoutExtension($InfName).ToLowerInvariant() + '.cat')
                CerPath          = Join-Path $candidateRoot $CerName
            }
        }
    }

    throw "Unable to locate $InfName and $SysName under $PackageRoot or $PackageRoot\package."
}

function Sync-SignedPackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackageRoot,

        [Parameter(Mandatory = $true)]
        [string]$InfName,

        [Parameter(Mandatory = $true)]
        [string]$SysName,

        [Parameter(Mandatory = $true)]
        [string]$CerName,

        [Parameter(Mandatory = $true)]
        [System.Security.Cryptography.X509Certificates.X509Certificate2]$SigningCertificate
    )

    $package = Resolve-DriverPackageRoot -PackageRoot $PackageRoot -InfName $InfName -SysName $SysName -CerName $CerName

    foreach ($staleCatPath in @($package.CatPath, $package.LowerCatPath)) {
        if (Test-Path -LiteralPath $staleCatPath) {
            Remove-Item -LiteralPath $staleCatPath -Force
        }
    }

    Get-ChildItem -LiteralPath $package.SourceRoot -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @('.pdb', '.map') } |
        Remove-Item -Force

    & $inf2cat "/driver:$($package.SourceRoot)" '/os:10_X64'
    if ($LASTEXITCODE -ne 0) {
        throw "Inf2Cat failed for $($package.SourceRoot)"
    }

    if (-not (Test-Path -LiteralPath $package.CatPath) -and (Test-Path -LiteralPath $package.LowerCatPath)) {
        Move-Item -LiteralPath $package.LowerCatPath -Destination $package.CatPath -Force
    }

    if (-not (Test-Path -LiteralPath $package.CatPath)) {
        throw "Generated catalog not found after Inf2Cat: $($package.CatPath)"
    }

    Invoke-SignCommand -Arguments @(
        'sign',
        '/sha1',
        $SigningCertificate.Thumbprint,
        '/fd',
        'sha256',
        '/tr',
        $timestampUrl,
        '/td',
        'sha256',
        $package.SysPath
    )

    Invoke-SignCommand -Arguments @(
        'sign',
        '/sha1',
        $SigningCertificate.Thumbprint,
        '/fd',
        'sha256',
        '/tr',
        $timestampUrl,
        '/td',
        'sha256',
        $package.CatPath
    )

    Export-Certificate -Cert $SigningCertificate -FilePath $package.CerPath -Force | Out-Null

    if (-not (Test-Path -LiteralPath $package.MirrorRoot)) {
        New-Item -ItemType Directory -Force -Path $package.MirrorRoot | Out-Null
    }

    foreach ($artifactPath in @(
        $package.InfPath,
        $package.SysPath,
        $package.CatPath,
        $package.CerPath
    )) {
        Copy-Item -LiteralPath $artifactPath -Destination (Join-Path $package.MirrorRoot (Split-Path -Leaf $artifactPath)) -Force
    }

    Write-Host "Signed package ready: $($package.SourceRoot)"
    Get-ChildItem -LiteralPath $package.SourceRoot -File |
        Where-Object { $_.Name -in @($InfName, $SysName, [System.IO.Path]::GetFileName($package.CatPath), $CerName) } |
        Select-Object Name, Length

    if ($package.MirrorRoot -ne $package.SourceRoot) {
        Write-Host "Mirrored signed package: $($package.MirrorRoot)"
        Get-ChildItem -LiteralPath $package.MirrorRoot -File |
            Where-Object { $_.Name -in @($InfName, $SysName, [System.IO.Path]::GetFileName($package.CatPath), $CerName) } |
            Select-Object Name, Length
    }
}

$signingCertificate = Resolve-SigningCertificate -PfxPath $certificatePath -Password $certificatePassword

Sync-SignedPackage -PackageRoot (Join-Path $stageRoot 'driver') -InfName 'GaYmFilter.inf' -SysName 'GaYmFilter.sys' -CerName 'GaYmFilter.cer' -SigningCertificate $signingCertificate
Sync-SignedPackage -PackageRoot (Join-Path $stageRoot 'upper') -InfName 'GaYmXInputFilter.inf' -SysName 'GaYmXInputFilter.sys' -CerName 'GaYmXInputFilter.cer' -SigningCertificate $signingCertificate
