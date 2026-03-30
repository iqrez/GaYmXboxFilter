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

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-15d30-micromap.txt'
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
    '0x00015D30' = Load-Record -RepoRoot $repoRoot -Target '0x00015D30'
    '0x0002F368' = Load-Record -RepoRoot $repoRoot -Target '0x0002F368'
    '0x00016220' = Load-Record -RepoRoot $repoRoot -Target '0x00016220'
    '0x000163D8' = Load-Record -RepoRoot $repoRoot -Target '0x000163D8'
    '0x00058B00' = Load-Record -RepoRoot $repoRoot -Target '0x00058B00'
}

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI 15D30 Micro Map') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Primary Body' -Body @(
    Format-RecordSummary -Record $records['0x00015D30'] -Class 'transfer-event-leaf'
)

Add-Section -Lines $lines -Title 'Direct Transfer Surface' -Body @(
    '- transfer imports exposed directly on 0x00015D30:',
    '  - KeAcquireSpinLockRaiseToDpc',
    '  - KeReleaseSpinLock',
    '  - IoFreeMdl',
    '  - KeLowerIrql',
    '  - KfRaiseIrql'
)

Add-Section -Lines $lines -Title 'Direct Branch Groups' -Body @(
    Format-RecordSummary -Record $records['0x0002F368'] -Class 'trace-leg'
    Format-RecordSummary -Record $records['0x00016220'] -Class 'trace-leg'
    Format-RecordSummary -Record $records['0x000163D8'] -Class 'mdl-side-leg'
    Format-RecordSummary -Record $records['0x00058B00'] -Class 'thunk'
)

Add-Section -Lines $lines -Title 'Interpretation' -Body @(
    '- 0x00015D30 is a real transfer-side leaf body, not just a bridge: it keeps the strongest spinlock and IRQL transition surface left on the transfer side',
    '- unlike the controller bodies, it does not expose timer or wait orchestration',
    '- its direct descendants mostly demote into trace, MDL-side cleanup, or a thunk, so 0x00015D30 itself remains the right transfer-side study target',
    '- recommended transfer micro-map: keep 0x00015D30 as the primary transfer event leaf and treat 0x0002F368 plus 0x00016220 as trace-side context rather than deeper cores'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
