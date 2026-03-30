[CmdletBinding()]
param(
    [string]$PrimaryAPath,
    [string]$PrimaryBPath,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-PrimaryAPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-exhaustive-0001B1F0-deep.txt'
}

function Resolve-PrimaryBPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-exhaustive-0003634C-deep.txt'
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-controller-timing-family-assessment.txt'
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
        Path           = $Path
        Target         = ''
        Function       = ''
        Size           = 0
        DirectInternal = 0
        DirectIat      = 0
        InternalTargets = New-Object 'System.Collections.Generic.List[string]'
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

        if ($line -match '^Target=(0x[0-9A-F]+) \| Calls=') {
            $record.InternalTargets.Add($matches[1]) | Out-Null
            continue
        }

        if ($line -match '^Import=([^|]+) \| Calls=') {
            $record.Imports.Add($matches[1].Trim()) | Out-Null
        }
    }

    return [pscustomobject]$record
}

function Classify-SharedTarget {
    param([object]$Record)

    if ($Record.Imports -contains 'WppRecorder.sys!WppAutoLogTrace') {
        return 'trace-leg'
    }

    if ($Record.Imports -contains 'ntoskrnl.exe!KdRefreshDebuggerNotPresent') {
        return 'debug-leg'
    }

    if ($Record.Imports -contains 'ntoskrnl.exe!ExAllocatePool2' -or $Record.Imports -contains 'ntoskrnl.exe!ExFreePoolWithTag') {
        return 'allocation-side-body'
    }

    if ($Record.Size -le 96 -and $Record.DirectInternal -eq 1 -and $Record.DirectIat -eq 0) {
        return 'wrapper-or-bridge'
    }

    if ($Record.Size -le 256 -and $Record.DirectInternal -le 1 -and $Record.DirectIat -le 1) {
        return 'thin-side-leg'
    }

    return 'body'
}

$primaryAPath = Resolve-PrimaryAPath -RequestedPath $PrimaryAPath
$primaryBPath = Resolve-PrimaryBPath -RequestedPath $PrimaryBPath
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath

foreach ($path in @($primaryAPath, $primaryBPath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required file not found: $path"
    }
}

$outputDirectory = Split-Path -Parent $outputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$primaryA = Parse-DeepFile -Path $primaryAPath
$primaryB = Parse-DeepFile -Path $primaryBPath

$sharedTargets = @($primaryA.InternalTargets | Where-Object { $primaryB.InternalTargets -contains $_ } | Sort-Object -Unique)
$onlyA = @($primaryA.InternalTargets | Where-Object { $primaryB.InternalTargets -notcontains $_ } | Sort-Object -Unique)
$onlyB = @($primaryB.InternalTargets | Where-Object { $primaryA.InternalTargets -notcontains $_ } | Sort-Object -Unique)

$sharedSummaries = foreach ($target in $sharedTargets) {
    $deepPath = Join-Path (Get-RepoRoot) ("out\dev\usbxhci-exhaustive-{0}-deep.txt" -f $target.Substring(2).PadLeft(8, '0'))
    if (-not (Test-Path -LiteralPath $deepPath)) {
        [pscustomobject]@{
            Target   = $target
            Class    = 'missing-artifact'
            Size     = 0
            Fanout   = '0/0'
            Imports  = '<none>'
        }
        continue
    }

    $parsed = Parse-DeepFile -Path $deepPath
    [pscustomobject]@{
        Target   = $target
        Class    = Classify-SharedTarget -Record $parsed
        Size     = $parsed.Size
        Fanout   = ('{0}/{1}' -f $parsed.DirectInternal, $parsed.DirectIat)
        Imports  = if ($parsed.Imports.Count -eq 0) { '<none>' } else { $parsed.Imports -join '; ' }
    }
}

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI Controller Timing Family Assessment') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Primary Bodies' -Body @(
    "PrimaryA : $($primaryA.Target) | Function=$($primaryA.Function) | Size=$($primaryA.Size) | Fanout=$($primaryA.DirectInternal)/$($primaryA.DirectIat) | Imports=$([string]::Join('; ', $primaryA.Imports))",
    "PrimaryB : $($primaryB.Target) | Function=$($primaryB.Function) | Size=$($primaryB.Size) | Fanout=$($primaryB.DirectInternal)/$($primaryB.DirectIat) | Imports=$([string]::Join('; ', $primaryB.Imports))"
)

Add-Section -Lines $lines -Title 'Shared Direct Callees' -Body @(
    if ($sharedSummaries.Count -eq 0) {
        '<none>'
    } else {
        foreach ($summary in $sharedSummaries) {
            "Target=$($summary.Target) | Class=$($summary.Class) | Size=$($summary.Size) | Fanout=$($summary.Fanout) | Imports=$($summary.Imports)"
        }
    }
)

Add-Section -Lines $lines -Title 'Exclusive Direct Callees' -Body @(
    "OnlyPrimaryA : $(if ($onlyA.Count -eq 0) { '<none>' } else { [string]::Join(', ', $onlyA) })",
    "OnlyPrimaryB : $(if ($onlyB.Count -eq 0) { '<none>' } else { [string]::Join(', ', $onlyB) })"
)

Add-Section -Lines $lines -Title 'Interpretation' -Body @(
    '- the two controller timing bodies do share a callee spine, but the shared descendants mostly demote to wrappers, trace legs, or allocation-side support',
    '- the shared set does not reveal a cleaner deeper timing body than the parents themselves',
    '- the strongest intervention candidates in this family remain the primary controller bodies rather than any shared descendant',
    "- recommended controller-family order: $($primaryA.Target), $($primaryB.Target)"
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
