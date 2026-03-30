[CmdletBinding()]
param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-1b1f0-micromap.txt'
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
        [string]$Class
    )

    $imports = if ($Record.Imports.Count -eq 0) { '<none>' } else { $Record.Imports -join '; ' }
    return "Target=$($Record.Target) | Class=$Class | Function=$($Record.Function) | Size=$($Record.Size) | Fanout=$($Record.DirectInternal)/$($Record.DirectIat) | Imports=$imports"
}

$repoRoot = Get-RepoRoot
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath

$records = @{
    '0x0001B1F0' = Load-Record -RepoRoot $repoRoot -Target '0x0001B1F0'
    '0x0003FC38' = Load-Record -RepoRoot $repoRoot -Target '0x0003FC38'
    '0x0000D210' = Load-Record -RepoRoot $repoRoot -Target '0x0000D210'
    '0x00019AC8' = Load-Record -RepoRoot $repoRoot -Target '0x00019AC8'
    '0x0001A724' = Load-Record -RepoRoot $repoRoot -Target '0x0001A724'
    '0x0001BA28' = Load-Record -RepoRoot $repoRoot -Target '0x0001BA28'
    '0x0001BA64' = Load-Record -RepoRoot $repoRoot -Target '0x0001BA64'
    '0x0001BA8C' = Load-Record -RepoRoot $repoRoot -Target '0x0001BA8C'
    '0x0002E390' = Load-Record -RepoRoot $repoRoot -Target '0x0002E390'
    '0x0000BE64' = Load-Record -RepoRoot $repoRoot -Target '0x0000BE64'
    '0x0000BF40' = Load-Record -RepoRoot $repoRoot -Target '0x0000BF40'
    '0x0000C970' = Load-Record -RepoRoot $repoRoot -Target '0x0000C970'
    '0x00058B00' = Load-Record -RepoRoot $repoRoot -Target '0x00058B00'
}

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI 1B1F0 Micro Map') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Primary Body' -Body @(
    Format-RecordSummary -Record $records['0x0001B1F0'] -Class 'controller-timing-body'
)

Add-Section -Lines $lines -Title 'Direct Timing Surface' -Body @(
    '- timing imports exposed directly on 0x0001B1F0:',
    '  - ntoskrnl.exe!KeQueryUnbiasedInterruptTime',
    '  - HAL.dll!KeStallExecutionProcessor',
    '  - ntoskrnl.exe!ExAllocateTimer',
    '  - ntoskrnl.exe!ExSetTimer',
    '  - ntoskrnl.exe!ExDeleteTimer',
    '  - ntoskrnl.exe!KeInitializeEvent',
    '  - ntoskrnl.exe!KeWaitForSingleObject',
    '  - ntoskrnl.exe!KeGetCurrentIrql'
)

Add-Section -Lines $lines -Title 'Direct Branch Groups' -Body @(
    Format-RecordSummary -Record $records['0x0003FC38'] -Class 'timing-descendant'
    Format-RecordSummary -Record $records['0x0000D210'] -Class 'bridge-into-d258-path'
    Format-RecordSummary -Record $records['0x00019AC8'] -Class 'debug-side-leg'
    Format-RecordSummary -Record $records['0x0001A724'] -Class 'trace-leg'
    Format-RecordSummary -Record $records['0x0001BA28'] -Class 'wrapper-ladder'
    Format-RecordSummary -Record $records['0x0001BA64'] -Class 'wrapper-ladder'
    Format-RecordSummary -Record $records['0x0001BA8C'] -Class 'wrapper-ladder-self-loop'
    Format-RecordSummary -Record $records['0x0002E390'] -Class 'trace-leg'
    Format-RecordSummary -Record $records['0x0000BE64'] -Class 'wrapper-to-trace'
    Format-RecordSummary -Record $records['0x0000BF40'] -Class 'debug-irql-leg'
    Format-RecordSummary -Record $records['0x0000C970'] -Class 'debug-irql-leg'
    Format-RecordSummary -Record $records['0x00058B00'] -Class 'thunk'
)

Add-Section -Lines $lines -Title 'Interpretation' -Body @(
    '- 0x0001B1F0 is a real timing body, not just a dispatcher: its direct import surface already contains the strongest timer and wait primitives seen in the current shortlist',
    '- 0x0003FC38 is the only direct descendant that preserves timing-centric behavior and remains the best internal follow-on',
    '- 0x0000D210 is only a bridge into the secondary D258 path, not a competing timing core',
    '- 0x00019AC8, 0x0001A724, 0x0002E390, 0x0000BF40, and 0x0000C970 demote to debug or trace-side context',
    '- 0x0001BA28, 0x0001BA64, and 0x0001BA8C are only a wrapper ladder',
    '- recommended controller micro-map: 0x0001B1F0 as the primary timing body, 0x0003FC38 as the only meaningful direct timing descendant, and 0x0000D210 as the alternate bridge path'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
