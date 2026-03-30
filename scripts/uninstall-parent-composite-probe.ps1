[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated PowerShell session.'
    }
}

Assert-Administrator

$driverLines = & pnputil /enum-drivers
if ($LASTEXITCODE -ne 0) {
    throw 'Failed to enumerate DriverStore packages.'
}

$records = @()
$current = $null

foreach ($line in $driverLines) {
    if ($line -match '^\s*Published Name:\s*(.+)$') {
        if ($current) {
            $records += [pscustomobject]$current
        }

        $current = @{
            PublishedName = $Matches[1].Trim()
            OriginalName  = ''
        }
        continue
    }

    if ($current -and $line -match '^\s*Original Name:\s*(.+)$') {
        $current.OriginalName = $Matches[1].Trim()
    }
}

if ($current) {
    $records += [pscustomobject]$current
}

$probePackages = $records | Where-Object { $_.OriginalName -ieq 'GaYmCompositeProbe.inf' }

if (-not $probePackages) {
    Write-Host 'No published composite parent probe packages were found.'
    return
}

foreach ($package in $probePackages) {
    Write-Host "Removing $($package.PublishedName) ($($package.OriginalName))"
    $output = & pnputil /delete-driver $package.PublishedName /uninstall /force
    $output | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to remove $($package.PublishedName)."
    }
}
