[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$CertThumbprint = '3DAECD2590086E2751C2AC94C24CCEC485A662E0'
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

if (-not (Test-Path -LiteralPath $inf2cat)) {
    throw "Inf2Cat.exe not found: $inf2cat"
}
if (-not (Test-Path -LiteralPath $signtool)) {
    throw "signtool.exe not found: $signtool"
}

$cert = Get-ChildItem Cert:\CurrentUser\My,Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
    Where-Object { $_.Thumbprint -eq $CertThumbprint } |
    Select-Object -First 1
if (-not $cert) {
    throw "Signing certificate not found in CurrentUser\\My or LocalMachine\\My: $CertThumbprint"
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

function New-SignedDriverPackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackageDirectory,
        [Parameter(Mandatory = $true)]
        [string]$InfName,
        [Parameter(Mandatory = $true)]
        [string]$SysName,
        [Parameter(Mandatory = $true)]
        [string]$CerName
    )

    $infPath = Join-Path $PackageDirectory $InfName
    $sysPath = Join-Path $PackageDirectory $SysName
    $catPath = [System.IO.Path]::ChangeExtension($infPath, '.cat')
    $lowercaseCatPath = Join-Path $PackageDirectory ([System.IO.Path]::GetFileNameWithoutExtension($InfName).ToLowerInvariant() + '.cat')
    $cerPath = Join-Path $PackageDirectory $CerName

    if (-not (Test-Path -LiteralPath $infPath)) {
        throw "Driver INF not found: $infPath"
    }
    if (-not (Test-Path -LiteralPath $sysPath)) {
        throw "Driver SYS not found: $sysPath"
    }

    foreach ($staleCat in @($catPath, $lowercaseCatPath)) {
        if (Test-Path -LiteralPath $staleCat) {
            Remove-Item -LiteralPath $staleCat -Force
        }
    }
    Get-ChildItem -LiteralPath $PackageDirectory -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @('.pdb', '.map') } |
        Remove-Item -Force

    & $inf2cat "/driver:$PackageDirectory" '/os:10_X64'
    if ($LASTEXITCODE -ne 0) {
        throw "Inf2Cat failed for $PackageDirectory"
    }

    if (-not (Test-Path -LiteralPath $catPath) -and (Test-Path -LiteralPath $lowercaseCatPath)) {
        Move-Item -LiteralPath $lowercaseCatPath -Destination $catPath -Force
    }
    if (-not (Test-Path -LiteralPath $catPath)) {
        throw "Generated catalog not found after Inf2Cat: $catPath"
    }

    Invoke-SignCommand -Arguments @('sign', '/sha1', $CertThumbprint, '/fd', 'sha256', '/tr', 'http://timestamp.digicert.com', '/td', 'sha256', $sysPath)
    Invoke-SignCommand -Arguments @('sign', '/sha1', $CertThumbprint, '/fd', 'sha256', '/tr', 'http://timestamp.digicert.com', '/td', 'sha256', $catPath)

    Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null

    Write-Host "Signed package ready: $PackageDirectory"
    Get-ChildItem -LiteralPath $PackageDirectory -File |
        Where-Object { $_.Name -in @($InfName, $SysName, [System.IO.Path]::GetFileName($catPath), $CerName) } |
        Select-Object Name, Length
}

New-SignedDriverPackage -PackageDirectory (Join-Path $stageRoot 'driver') -InfName 'GaYmFilter.inf' -SysName 'GaYmFilter.sys' -CerName 'GaYmFilter.cer'
New-SignedDriverPackage -PackageDirectory (Join-Path $stageRoot 'upper') -InfName 'GaYmXInputFilter.inf' -SysName 'GaYmXInputFilter.sys' -CerName 'GaYmXInputFilter.cer'
