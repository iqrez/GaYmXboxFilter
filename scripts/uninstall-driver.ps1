[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common-paths.ps1')

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run uninstall-driver.ps1 from an elevated PowerShell session.'
    }
}

function Get-GaYmDriverRecords {
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

    $records = @()
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
            $records += [pscustomobject]@{
                PublishedName = $publishedName
                OriginalName = $originalName
            }
        }
    }

    return $records
}

function Get-HidChildInstanceId {
    $device = Get-PnpDevice -Class HIDClass -ErrorAction Stop |
        Where-Object { $_.InstanceId -like 'HID\VID_045E&PID_02FF&IG_00*' -and ($_.Present -eq $true -or $_.Status -eq 'OK') } |
        Select-Object -First 1

    if ($device) {
        return $device.InstanceId
    }

    return $null
}

function Get-LiveStackText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstanceId
    )

    $pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
    $stackOutput = & $pnputil /enum-devices /instanceid $InstanceId /stack /drivers 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw ("pnputil failed while querying the live stack for $InstanceId.`n" + ($stackOutput -join [Environment]::NewLine))
    }

    return ($stackOutput -join [Environment]::NewLine)
}

function Get-StackDriverNames {
    param(
        [Parameter(Mandatory = $true)]
        [string]$StackText
    )

    $stackNames = New-Object 'System.Collections.Generic.List[string]'
    $capturing = $false

    foreach ($line in ($StackText -split "\r?\n")) {
        if (-not $capturing) {
            if ($line -match '^\s*Stack:\s*(.+?)\s*$') {
                $stackNames.Add($matches[1].Trim())
                $capturing = $true
            }
            continue
        }

        if ($line -match '^\s*Matching Drivers:\s*$' -or $line -match '^\s*$') {
            break
        }

        if ($line -match '^\s+(.+?)\s*$') {
            $stackNames.Add($matches[1].Trim())
            continue
        }

        break
    }

    return $stackNames.ToArray()
}

Assert-Administrator

$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$repoRoot = Split-Path -Parent $PSScriptRoot
$layout = Get-GaYmArtifactLayout -Root $repoRoot
$installStatePath = New-GaYmStatePath -Layout $layout -LeafName 'install-driver-state.json'
$driverRecords = Get-GaYmDriverRecords
$hidChild = Get-HidChildInstanceId

if ($driverRecords.Count -eq 0) {
    if (Test-Path $installStatePath) {
        Remove-Item -Path $installStatePath -Force
    }
    Write-Host 'No published GaYm driver packages were found.'
    return
}

$orderedRecords = $driverRecords | Sort-Object @{ Expression = { if ($_.OriginalName -ieq 'GaYmXboxFilter.inf') { 0 } else { 1 } } }, PublishedName

foreach ($record in $orderedRecords) {
    Write-Host "=== Removing $($record.PublishedName) ($($record.OriginalName)) ==="
    $output = & $pnputil /delete-driver $record.PublishedName /uninstall /force 2>&1
    $exitCode = $LASTEXITCODE
    $outputText = $output -join [Environment]::NewLine
    if ($outputText) {
        Write-Host $outputText
    }

    $deleted = $outputText -match 'Driver package deleted successfully'
    $rebootRequired = $outputText -match 'System reboot is needed'
    if (-not $deleted -and $exitCode -ne 0 -and -not $rebootRequired) {
        throw "Failed to remove $($record.PublishedName)."
    }
}

$remainingRecords = Get-GaYmDriverRecords

Write-Host ''
if ($remainingRecords.Count -eq 0) {
    if (Test-Path $installStatePath) {
        Remove-Item -Path $installStatePath -Force
    }
    Write-Host 'GaYm packages removed from DriverStore.'
} else {
    Write-Warning ("Some GaYm packages still remain in DriverStore: {0}" -f (($remainingRecords | ForEach-Object { $_.PublishedName }) -join ', '))
}

if ($hidChild) {
    try {
        $stackText = Get-LiveStackText -InstanceId $hidChild
        $stackNames = Get-StackDriverNames -StackText $stackText
        Write-Host ("Post-uninstall stack: {0}" -f ($stackNames -join ' -> '))
        if ($stackNames -contains 'GaYmXInputFilter' -or $stackNames -contains 'GaYmFilter') {
            Write-Warning 'The live HID child still shows GaYm filter entries. Reboot if Windows reports pending device configuration.'
        }
    } catch {
        Write-Warning "Could not verify the live HID child stack after uninstall: $($_.Exception.Message)"
    }
}

Write-Host 'Reboot if Windows reports pending device configuration.'
