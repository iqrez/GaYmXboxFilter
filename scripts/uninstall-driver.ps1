[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$HardwareId = 'HID\VID_045E&PID_02FF&IG_00',
    [string]$InstanceId,
    [string]$DriverInf,
    [string]$UpperDriverInf
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$profileSegment = if ($Configuration -eq 'Debug') { 'dev' } else { 'release' }
if (-not $DriverInf) {
    $DriverInf = Join-Path $repoRoot ("out\" + $profileSegment + '\driver\GaYmFilter.inf')
}
if (-not $UpperDriverInf) {
    $UpperDriverInf = Join-Path $repoRoot ("out\" + $profileSegment + '\upper\GaYmXInputFilter.inf')
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run uninstall-driver.ps1 from an elevated PowerShell prompt.'
}

$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
if (-not (Test-Path -LiteralPath $pnputil)) {
    throw "pnputil.exe not found: $pnputil"
}

function Get-PublishedNamesFromInf {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InfPath
    )

    $fileName = [System.IO.Path]::GetFileName($InfPath)
    $enumOutput = & $pnputil /enum-drivers 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "pnputil failed while enumerating installed drivers."
    }

    $publishedNameMatches = New-Object System.Collections.Generic.List[string]
    $currentPublishedName = $null
    $currentOriginalName = $null

    foreach ($line in $enumOutput) {
        if ($line -match '^\s*Published Name:\s*(.+)$') {
            $currentPublishedName = $Matches[1].Trim()
            continue
        }

        if ($line -match '^\s*Original Name:\s*(.+)$') {
            $currentOriginalName = $Matches[1].Trim()
            continue
        }

        if ([string]::IsNullOrWhiteSpace($line)) {
            if ($currentPublishedName -and $currentOriginalName -and ($currentOriginalName -ieq $fileName)) {
                $publishedNameMatches.Add($currentPublishedName) | Out-Null
            }

            $currentPublishedName = $null
            $currentOriginalName = $null
        }
    }

    if ($currentPublishedName -and $currentOriginalName -and ($currentOriginalName -ieq $fileName)) {
        $publishedNameMatches.Add($currentPublishedName) | Out-Null
    }

    return $publishedNameMatches.ToArray()
}

function Remove-DriverPackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InfPath,

        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $publishedNames = Get-PublishedNamesFromInf -InfPath $InfPath
    if ($publishedNames.Count -eq 0) {
        Write-Host "$Label package not present in the driver store; skipping."
        return
    }

    foreach ($publishedName in $publishedNames) {
        Write-Host "Removing $Label package: $publishedName"
        $deleteOutput = & $pnputil /delete-driver $publishedName /uninstall /force 2>&1
        $deleteExitCode = $LASTEXITCODE
        $deleteText = $deleteOutput -join [Environment]::NewLine
        $rebootRequired = $deleteText -match 'System reboot is needed to complete uninstall operations!' -or
            $deleteText -match 'System reboot is needed to complete unconfiguration operations!'
        $deleteSucceeded = $deleteText -match 'Driver package deleted successfully\.'

        if ($deleteExitCode -ne 0 -and -not ($deleteSucceeded -and $rebootRequired)) {
            throw ("pnputil failed to delete $Label package $publishedName.`n" + $deleteText)
        }

        Write-Host $deleteText
    }
}

function Test-InstanceRequiresReboot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TargetInstanceId
    )

    $propertyOutput = & $pnputil /enum-devices /instanceid $TargetInstanceId /properties 2>&1
    if ($LASTEXITCODE -ne 0) {
        return $true
    }

    $propertyText = $propertyOutput -join [Environment]::NewLine
    return $propertyText -match 'DEVPKEY_Device_IsRebootRequired \[Boolean\]:\s*\r?\n\s*TRUE'
}

function Test-LiveStackRecovery {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TargetHardwareId
    )

    $deviceOutput = & $pnputil /enum-devices /connected /class HIDClass 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw 'pnputil failed while enumerating connected HIDClass devices.'
    }

    $instanceMatch = $deviceOutput |
        Select-String '^\s*Instance ID:\s*(.+)$' |
        Where-Object {
            $_.Matches[0].Groups[1].Value.Trim().StartsWith($TargetHardwareId, [StringComparison]::OrdinalIgnoreCase)
        } |
        Select-Object -First 1

    if (-not $instanceMatch) {
        Write-Warning "No connected HIDClass device matches $TargetHardwareId, so live stack verification was skipped."
        return
    }

    $resolvedInstanceId = $instanceMatch.Matches[0].Groups[1].Value.Trim()
    $stackOutput = & $pnputil /enum-devices /instanceid $resolvedInstanceId /services /stack /drivers 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw ("pnputil failed while querying the live stack for $resolvedInstanceId.`n" + ($stackOutput -join [Environment]::NewLine))
    }

    $stackText = $stackOutput -join [Environment]::NewLine
    Write-Host ''
    Write-Host "Live stack after uninstall for ${resolvedInstanceId}:"
    Write-Host $stackText
    Write-Host ''

    if ($stackText -match '(?im)^\s*GaYmFilter\s*$' -or $stackText -match '(?im)^\s*GaYmXInputFilter\s*$') {
        throw 'GaYm filter packages still appear in the live stack after uninstall.'
    }

    if ($stackText -notmatch '(?im)^\s*(?:Stack:\s*)?HidUsb\s*$') {
        throw 'HidUsb is not present in the live stack after uninstall.'
    }
}

Write-Host "Target hardware ID: $HardwareId"
Write-Host ''

if (Test-Path -LiteralPath $UpperDriverInf) {
    Remove-DriverPackage -InfPath $UpperDriverInf -Label 'upper'
    Write-Host ''
} else {
    Write-Host "Upper driver package not present at $UpperDriverInf; skipping upper removal."
    Write-Host ''
}

Remove-DriverPackage -InfPath $DriverInf -Label 'lower'
Write-Host ''

if (-not $InstanceId) {
    $deviceOutput = & $pnputil /enum-devices /connected /class HIDClass 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw 'pnputil failed while enumerating connected HIDClass devices.'
    }

    $instanceMatch = $deviceOutput |
        Select-String '^\s*Instance ID:\s*(.+)$' |
        Where-Object {
            $_.Matches[0].Groups[1].Value.Trim().StartsWith($HardwareId, [StringComparison]::OrdinalIgnoreCase)
        } |
        Select-Object -First 1

    if ($instanceMatch) {
        $InstanceId = $instanceMatch.Matches[0].Groups[1].Value.Trim()
    }
}

if ($InstanceId) {
    $restartOutput = & $pnputil /restart-device $InstanceId 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Warning ("pnputil could not restart $InstanceId after uninstall.`n" + ($restartOutput -join [Environment]::NewLine))
    } else {
        Write-Host "Restart output:"
        Write-Host ($restartOutput -join [Environment]::NewLine)
        Write-Host ''
    }

    if (Test-InstanceRequiresReboot -TargetInstanceId $InstanceId) {
        Write-Warning "Windows still reports that $InstanceId requires a reboot after uninstall. Reboot once, reconnect the controller, and rerun uninstall-driver.ps1 if you need live stack recovery verification."
        return
    }
}

Test-LiveStackRecovery -TargetHardwareId $HardwareId
Write-Host 'Uninstall complete. Controller recovery verified at the HIDUsb level where the device was present.'
