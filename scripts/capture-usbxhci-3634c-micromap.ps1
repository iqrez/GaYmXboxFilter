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

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-3634c-micromap.txt'
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
    '0x0003634C' = Load-Record -RepoRoot $repoRoot -Target '0x0003634C'
    '0x0000D210' = Load-Record -RepoRoot $repoRoot -Target '0x0000D210'
    '0x00019AC8' = Load-Record -RepoRoot $repoRoot -Target '0x00019AC8'
    '0x0001A724' = Load-Record -RepoRoot $repoRoot -Target '0x0001A724'
    '0x0001BA28' = Load-Record -RepoRoot $repoRoot -Target '0x0001BA28'
    '0x0002E390' = Load-Record -RepoRoot $repoRoot -Target '0x0002E390'
    '0x0001BA00' = Load-Record -RepoRoot $repoRoot -Target '0x0001BA00'
    '0x0001A7FC' = Load-Record -RepoRoot $repoRoot -Target '0x0001A7FC'
    '0x0002F834' = Load-Record -RepoRoot $repoRoot -Target '0x0002F834'
    '0x000303B4' = Load-Record -RepoRoot $repoRoot -Target '0x000303B4'
    '0x00041388' = Load-Record -RepoRoot $repoRoot -Target '0x00041388'
}

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI 3634C Micro Map') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Primary Body' -Body @(
    Format-RecordSummary -Record $records['0x0003634C'] -Class 'controller-timing-body'
)

Add-Section -Lines $lines -Title 'Direct Timing Surface' -Body @(
    '- timing imports exposed directly on 0x0003634C:',
    '  - ntoskrnl.exe!KeQueryUnbiasedInterruptTime',
    '  - ntoskrnl.exe!KeDelayExecutionThread',
    '  - ntoskrnl.exe!KeGetCurrentIrql'
)

Add-Section -Lines $lines -Title 'Shared Controller Spine' -Body @(
    Format-RecordSummary -Record $records['0x0000D210'] -Class 'bridge-into-d258-path'
    Format-RecordSummary -Record $records['0x00019AC8'] -Class 'debug-side-leg'
    Format-RecordSummary -Record $records['0x0001A724'] -Class 'trace-leg'
    Format-RecordSummary -Record $records['0x0001BA28'] -Class 'wrapper-ladder'
    Format-RecordSummary -Record $records['0x0002E390'] -Class 'trace-leg'
)

Add-Section -Lines $lines -Title 'Exclusive Descendants' -Body @(
    Format-RecordSummary -Record $records['0x0001BA00'] -Class 'wrapper-into-wrapper'
    Format-RecordSummary -Record $records['0x0001A7FC'] -Class 'trace-leg'
    Format-RecordSummary -Record $records['0x0002F834'] -Class 'bridge-to-etw-stub'
    Format-RecordSummary -Record $records['0x000303B4'] -Class 'bridge-to-etw-stub'
    Format-RecordSummary -Record $records['0x00041388'] -Class 'interrupt-side-bridge'
)

Add-Section -Lines $lines -Title 'Interpretation' -Body @(
    '- 0x0003634C is a real timing body, but its direct timing surface is narrower than 0x0001B1F0: interrupt time, delayed thread sleep, and IRQL',
    '- its shared descendants mostly reconnect into the same bridge, wrapper, trace, or debug-side controller spine seen elsewhere',
    '- its exclusive descendants do not reveal a better timing child body: they demote into wrapper, trace, bridge-to-stub, or interrupt-side context',
    '- recommended controller micro-map: keep 0x0003634C as the secondary timing body itself, not as a parent to a better deeper timing descendant'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
