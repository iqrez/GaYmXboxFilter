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

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-10440-helper-branch-assessment.txt'
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

function Classify-Node {
    param([object]$Record)

    switch ($Record.Target) {
        '0x00010D60' { return 'transfer-leaf-body' }
        '0x00058B00' { return 'thunk' }
        '0x00058EC0' { return 'opaque-slab' }
        '0x00011240' { return 'trace-leg' }
        '0x00022E7C' { return 'control-assert-leg' }
        '0x000076A0' { return 'trace-leg' }
        '0x00010CD8' { return 'stub-neighbor' }
        '0x000111C4' { return 'mdl-side-leg' }
        '0x00012700' { return 'trace-leg' }
        '0x000148B4' { return 'wrapper-side-leg' }
        '0x0002F21C' { return 'trace-leg' }
        '0x0003C8C4' { return 'side-bridge' }
        default { return 'side-node' }
    }
}

$repoRoot = Get-RepoRoot
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath
$targets = @(
    '0x00010D60',
    '0x00058B00',
    '0x00058EC0',
    '0x00011240',
    '0x00022E7C',
    '0x000076A0',
    '0x00010CD8',
    '0x000111C4',
    '0x00012700',
    '0x000148B4',
    '0x0002F21C',
    '0x0003C8C4'
)

$records = foreach ($target in $targets) {
    $path = Join-Path $repoRoot ("out\dev\usbxhci-exhaustive-{0}-deep.txt" -f $target.Substring(2).PadLeft(8, '0'))
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required deep artifact not found: $path"
    }

    $record = Parse-DeepFile -Path $path
    [pscustomobject]@{
        Target         = $record.Target
        Function       = $record.Function
        Size           = $record.Size
        DirectInternal = $record.DirectInternal
        DirectIat      = $record.DirectIat
        Imports        = @($record.Imports)
        Class          = Classify-Node -Record $record
    }
}

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI 10440 Helper Branch Assessment') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Branch Nodes' -Body @(
    foreach ($record in $records) {
        $imports = if ($record.Imports.Count -eq 0) { '<none>' } else { $record.Imports -join '; ' }
        "Target=$($record.Target) | Class=$($record.Class) | Function=$($record.Function) | Size=$($record.Size) | Fanout=$($record.DirectInternal)/$($record.DirectIat) | Imports=$imports"
    }
)

Add-Section -Lines $lines -Title 'Interpretation' -Body @(
    '- 0x00010D60 is the only substantive transfer-side descendant under 0x00010440',
    '- the other direct descendants demote into trace, control, stub, opaque, or side-bridge context',
    '- recommended result: keep 0x00010D60 as the leaf body for the helper-heavy transfer subfamily'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
