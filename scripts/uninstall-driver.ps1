[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run uninstall-driver.ps1 from an elevated PowerShell session.'
    }
}

function Get-GaYmPublishedNames {
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

    $publishedNames = @()
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
            $publishedNames += $publishedName
        }
    }

    return $publishedNames
}

Assert-Administrator

$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$publishedNames = Get-GaYmPublishedNames

if ($publishedNames.Count -eq 0) {
    Write-Host 'No published GaYm driver packages were found.'
    return
}

foreach ($publishedName in $publishedNames) {
    Write-Host "=== Removing $publishedName ==="
    & $pnputil /delete-driver $publishedName /uninstall /force
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to remove $publishedName."
    }
}

Write-Host ''
Write-Host 'GaYm packages removed. Reboot if Windows reports pending device configuration.'

