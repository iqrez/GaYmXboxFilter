[CmdletBinding()]
param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    $scriptRootParent = Split-Path -Parent $PSScriptRoot
    if (Test-Path -LiteralPath (Join-Path $scriptRootParent '.git')) {
        return $scriptRootParent
    }

    $currentLocation = (Get-Location).Path
    if (Test-Path -LiteralPath (Join-Path $currentLocation '.git')) {
        return $currentLocation
    }

    return $scriptRootParent
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-controller-core-compare.txt'
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

function Parse-DeepFile {
    param([string]$Path)

    $content = Get-Content -Path $Path
    $record = [ordered]@{
        Target         = ''
        Function       = ''
        Size           = 0
        DirectInternal = 0
        DirectIat      = 0
        Imports        = New-Object 'System.Collections.Generic.List[string]'
    }

    foreach ($line in $content) {
        if ($line -match '^TargetRva\s+:\s+(0x[0-9A-F]+)$') {
            $record.Target = $matches[1]
            continue
        }

        if ($line -match '^Function\s+:\s+(0x[0-9A-F]+-0x[0-9A-F]+)$') {
            $record.Function = $matches[1]
            continue
        }

        if ($line -match '^Size\s+:\s+(\d+)$') {
            $record.Size = [int]$matches[1]
            continue
        }

        if ($line -match '^DirectInternal\s+:\s+(\d+)$') {
            $record.DirectInternal = [int]$matches[1]
            continue
        }

        if ($line -match '^DirectIat\s+:\s+(\d+)$') {
            $record.DirectIat = [int]$matches[1]
            continue
        }

        if ($line -match '^Import=([^|]+) \| Calls=') {
            $record.Imports.Add($matches[1].Trim()) | Out-Null
        }
    }

    return [pscustomobject]$record
}

function Load-Record {
    param(
        [string]$RepoRoot,
        [string]$Target
    )

    $path = Join-Path $RepoRoot ("out\dev\usbxhci-exhaustive-{0}-deep.txt" -f $Target.Substring(2).PadLeft(8, '0'))
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required deep artifact not found: $path"
    }

    return Parse-DeepFile -Path $path
}

function Format-RecordSummary {
    param(
        [object]$Record,
        [string]$Label
    )

    $imports = if ($Record.Imports.Count -eq 0) { '<none>' } else { $Record.Imports -join '; ' }
    return "Target=$($Record.Target) | Label=$Label | Function=$($Record.Function) | Size=$($Record.Size) | Fanout=$($Record.DirectInternal)/$($Record.DirectIat) | Imports=$imports"
}

$repoRoot = Get-RepoRoot
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath

$records = @{
    '0x0001B1F0' = Load-Record -RepoRoot $repoRoot -Target '0x0001B1F0'
    '0x0003FC38' = Load-Record -RepoRoot $repoRoot -Target '0x0003FC38'
    '0x0003634C' = Load-Record -RepoRoot $repoRoot -Target '0x0003634C'
}

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI Controller Core Compare') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Core Bodies' -Body @(
    Format-RecordSummary -Record $records['0x0001B1F0'] -Label 'primary-controller-core'
    Format-RecordSummary -Record $records['0x0003634C'] -Label 'secondary-controller-core'
    Format-RecordSummary -Record $records['0x0003FC38'] -Label 'primary-direct-descendant'
)

Add-Section -Lines $lines -Title 'Timing Surface Comparison' -Body @(
    '- 0x0001B1F0 direct timing surface:',
    '  - KeQueryUnbiasedInterruptTime',
    '  - KeStallExecutionProcessor',
    '  - ExAllocateTimer',
    '  - ExSetTimer',
    '  - ExDeleteTimer',
    '  - KeInitializeEvent',
    '  - KeWaitForSingleObject',
    '  - KeGetCurrentIrql',
    '- 0x0003634C direct timing surface:',
    '  - KeQueryUnbiasedInterruptTime',
    '  - KeDelayExecutionThread',
    '  - KeGetCurrentIrql',
    '- 0x0003FC38 direct timing surface:',
    '  - KeQueryPerformanceCounter',
    '  - KeGetCurrentIrql',
    '  - KeLowerIrql',
    '  - KfRaiseIrql'
)

Add-Section -Lines $lines -Title 'Comparison' -Body @(
    '- 0x0001B1F0 is broader and more orchestrative:',
    '  - timer allocation/set/delete',
    '  - active wait/event coordination',
    '  - explicit stall timing',
    '- 0x0003634C is narrower and more pacing-oriented:',
    '  - interrupt time',
    '  - delayed thread sleep',
    '  - IRQL awareness',
    '- 0x0003FC38 is not a peer controller core:',
    '  - it is the only meaningful direct timing descendant under 0x0001B1F0',
    '  - it looks like a tighter inner timing helper, not a second orchestrator'
)

Add-Section -Lines $lines -Title 'Result' -Body @(
    '- primary controller timing core: 0x0001B1F0',
    '- primary inner timing descendant: 0x0003FC38',
    '- secondary controller timing core: 0x0003634C',
    '- recommendation:',
    '  - if deeper host timing work continues, study 0x0001B1F0 first',
    '  - use 0x0003634C as the cleaner contrast body rather than as the main next target'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
