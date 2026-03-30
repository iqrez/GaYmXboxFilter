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

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-3fc38-branch-assessment.txt'
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

function Classify-BranchNode {
    param([object]$Record)

    if ($Record.Target -eq '0x00058B00') {
        return 'thunk'
    }

    if ($Record.Imports -contains 'WppRecorder.sys!WppAutoLogTrace') {
        return 'trace-leg'
    }

    if ($Record.Imports -contains 'ntoskrnl.exe!KdRefreshDebuggerNotPresent') {
        return 'debug-irql-leg'
    }

    if ($Record.Imports -contains 'ntoskrnl.exe!KeFlushQueuedDpcs') {
        return 'flush-side-leg'
    }

    if ($Record.Imports -contains 'ntoskrnl.exe!IofCompleteRequest') {
        return 'completion-side-leg'
    }

    if ($Record.Imports -contains 'HAL.dll!KeQueryPerformanceCounter') {
        return 'timing-body'
    }

    if ($Record.Size -le 96 -and $Record.DirectInternal -eq 1 -and $Record.DirectIat -eq 0) {
        return 'wrapper-or-bridge'
    }

    return 'side-body'
}

$repoRoot = Get-RepoRoot
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath
$targets = @(
    '0x0003FC38',
    '0x00012BF0',
    '0x0001BA28',
    '0x0000BF40',
    '0x0000C970',
    '0x0000D210',
    '0x0003C400',
    '0x0003FE84',
    '0x0003FF40',
    '0x00058B00'
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
        Class          = Classify-BranchNode -Record $record
    }
}

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI 3FC38 Branch Assessment') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Branch Nodes' -Body @(
    foreach ($record in $records) {
        $imports = if ($record.Imports.Count -eq 0) { '<none>' } else { $record.Imports -join '; ' }
        "Target=$($record.Target) | Class=$($record.Class) | Function=$($record.Function) | Size=$($record.Size) | Fanout=$($record.DirectInternal)/$($record.DirectIat) | Imports=$imports"
    }
)

Add-Section -Lines $lines -Title 'Interpretation' -Body @(
    '- 0x0003FC38 remains the timing-side body in this branch',
    '- 0x00012BF0 and 0x0003C400 demote to side bodies that immediately reconnect into trace or bridge machinery',
    '- 0x0003FE84 and 0x0003FF40 are service legs around DPC flush and request completion, not better timing cores',
    '- the remaining direct callees collapse to wrappers, debug legs, or a thunk',
    '- recommended result: keep 0x0003FC38 as the leaf timing body for the primary controller family'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
