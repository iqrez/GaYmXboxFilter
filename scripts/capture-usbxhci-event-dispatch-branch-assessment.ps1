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

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-event-dispatch-branch-assessment.txt'
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
        Internal       = New-Object 'System.Collections.Generic.List[string]'
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

        if ($line -match '^Target=(0x[0-9A-F]+) \| Calls=(\d+)') {
            $record.Internal.Add("$($matches[1]) ($($matches[2]))") | Out-Null
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
    $internal = if ($Record.Internal.Count -eq 0) { '<none>' } else { $Record.Internal -join '; ' }
    return "Target=$($Record.Target) | Class=$Class | Function=$($Record.Function) | Size=$($Record.Size) | Fanout=$($Record.DirectInternal)/$($Record.DirectIat) | Internal=$internal | Imports=$imports"
}

$repoRoot = Get-RepoRoot
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath

$records = @{
    '0x000038CC' = Load-Record -RepoRoot $repoRoot -Target '0x000038CC'
    '0x000077FC' = Load-Record -RepoRoot $repoRoot -Target '0x000077FC'
    '0x00003CBC' = Load-Record -RepoRoot $repoRoot -Target '0x00003CBC'
    '0x000042A0' = Load-Record -RepoRoot $repoRoot -Target '0x000042A0'
    '0x000220F0' = Load-Record -RepoRoot $repoRoot -Target '0x000220F0'
    '0x000137B4' = Load-Record -RepoRoot $repoRoot -Target '0x000137B4'
    '0x00022DA0' = Load-Record -RepoRoot $repoRoot -Target '0x00022DA0'
    '0x00015D30' = Load-Record -RepoRoot $repoRoot -Target '0x00015D30'
    '0x000163D8' = Load-Record -RepoRoot $repoRoot -Target '0x000163D8'
    '0x00007B70' = Load-Record -RepoRoot $repoRoot -Target '0x00007B70'
    '0x00008878' = Load-Record -RepoRoot $repoRoot -Target '0x00008878'
    '0x00008A50' = Load-Record -RepoRoot $repoRoot -Target '0x00008A50'
    '0x0003C6A0' = Load-Record -RepoRoot $repoRoot -Target '0x0003C6A0'
    '0x000355FC' = Load-Record -RepoRoot $repoRoot -Target '0x000355FC'
    '0x00058BC0' = Load-Record -RepoRoot $repoRoot -Target '0x00058BC0'
}

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI Event-Dispatch Branch Assessment') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Event Parents' -Body @(
    Format-RecordSummary -Record $records['0x000038CC'] -Class 'event-parent'
    Format-RecordSummary -Record $records['0x000077FC'] -Class 'event-parent'
)

Add-Section -Lines $lines -Title 'Shared Event Spine' -Body @(
    '- shared direct callees from both event parents:',
    '  - 0x00003C70: tiny stub',
    '  - 0x00003FA0: tiny thunk-like bridge',
    '  - 0x00004124: trace leg',
    '  - 0x000049B4: debug-side leg',
    '  - 0x00005BC0: reconnecting bridge into 0x00006A44',
    '- result:',
    '  - the shared event spine does not expose a better common deeper body'
)

Add-Section -Lines $lines -Title '0x000038CC Unique Branch' -Body @(
    Format-RecordSummary -Record $records['0x00003CBC'] -Class 'mixed-side-body'
    Format-RecordSummary -Record $records['0x000042A0'] -Class 'substantive-event-body'
    Format-RecordSummary -Record $records['0x000220F0'] -Class 'trace-leg'
    Format-RecordSummary -Record $records['0x000137B4'] -Class 'wrapper-leg'
    Format-RecordSummary -Record $records['0x00022DA0'] -Class 'stub'
    Format-RecordSummary -Record $records['0x00015D30'] -Class 'substantive-transfer-event-body'
    Format-RecordSummary -Record $records['0x000163D8'] -Class 'mdl-side-leg'
    Format-RecordSummary -Record $records['0x00058BC0'] -Class 'opaque-slab'
    '- interpretation:',
    '  - 0x00003CBC is mixed context and reconnects into helper-heavy/shared trace nodes',
    '  - 0x000042A0 is the first real unique event-side body',
    '  - under 0x000042A0, 0x00015D30 is the only substantive deeper body',
    '  - 0x00022DA0, 0x000163D8, and 0x00058BC0 do not outrank 0x00015D30'
)

Add-Section -Lines $lines -Title '0x000077FC Unique Branch' -Body @(
    Format-RecordSummary -Record $records['0x00007B70'] -Class 'trace-leg'
    Format-RecordSummary -Record $records['0x00008878'] -Class 'thin-side-body'
    Format-RecordSummary -Record $records['0x00008A50'] -Class 'stub'
    Format-RecordSummary -Record $records['0x0003C6A0'] -Class 'reconnecting-bridge'
    Format-RecordSummary -Record $records['0x000355FC'] -Class 'trace-leg'
    Format-RecordSummary -Record $records['0x00058BC0'] -Class 'opaque-slab'
    '- interpretation:',
    '  - 0x00007B70 reduces to trace',
    '  - 0x00008878 only fans into 0x00008A50 and trace-heavy 0x0001BF58 context',
    '  - 0x0003C6A0 reconnects into trace/bridge context rather than a clearer event body',
    '  - 0x000077FC does not expose a better unique deeper body than itself'
)

Add-Section -Lines $lines -Title 'Leaf Set' -Body @(
    '- helper-heavy transfer leaf set remains:',
    '  - 0x0001144D',
    '  - 0x00011E20',
    '  - leaf body 0x00010D60',
    '- event-dispatch transfer leaf set is now:',
    '  - 0x000038CC -> 0x000042A0 -> leaf body 0x00015D30',
    '  - 0x000077FC retained as the sibling event-side leaf',
    '- practical conclusion:',
    '  - 0x000038CC is the better event-dispatch family to study further if deeper timing work continues'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
