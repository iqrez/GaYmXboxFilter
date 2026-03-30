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

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-3634c-exclusive-assessment.txt'
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

    if ($Record.Imports -contains 'WppRecorder.sys!WppAutoLogTrace') {
        return 'trace-leg'
    }

    if ($Record.Target -eq '0x0001BA00') {
        return 'wrapper-ladder'
    }

    if ($Record.Target -eq '0x00056D58') {
        return 'wrapper-bridge'
    }

    if ($Record.Target -in @('0x0002F834', '0x000303B4')) {
        return 'side-bridge'
    }

    if ($Record.Target -eq '0x00041388') {
        return 'interrupt-side-leg'
    }

    return 'side-body'
}

$repoRoot = Get-RepoRoot
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath
$targets = @(
    '0x0001A7FC',
    '0x0001BA00',
    '0x0002F834',
    '0x000303B4',
    '0x00041388',
    '0x00056D58'
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
$lines.Add('# USBXHCI 3634C Exclusive Assessment') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Exclusive Nodes' -Body @(
    foreach ($record in $records) {
        $imports = if ($record.Imports.Count -eq 0) { '<none>' } else { $record.Imports -join '; ' }
        "Target=$($record.Target) | Class=$($record.Class) | Function=$($record.Function) | Size=$($record.Size) | Fanout=$($record.DirectInternal)/$($record.DirectIat) | Imports=$imports"
    }
)

Add-Section -Lines $lines -Title 'Interpretation' -Body @(
    '- the unique 0x0003634C branch does not reveal a better deeper timing body',
    '- it collapses into trace, wrapper, bridge, and interrupt-side context',
    '- 0x00056D58 only bridges into the already-demoted 0x00056DBC band',
    '- recommended result: keep 0x0003634C itself as the leaf timing body for the secondary controller family'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
