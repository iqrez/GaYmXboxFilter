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

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-reduced-leaf-shortlist.txt'
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

function Get-TargetLabel {
    param([string]$Target)

    switch ($Target) {
        '0x0001B1F0' { return 'controller-primary-body' }
        '0x0003FC38' { return 'controller-leaf-descendant' }
        '0x0003634C' { return 'controller-secondary-body' }
        '0x00010D60' { return 'transfer-helper-heavy-leaf' }
        '0x00015D30' { return 'transfer-event-leaf' }
        '0x000077FC' { return 'transfer-event-sibling-leaf' }
        default { return 'leaf-candidate' }
    }
}

function Get-ScoreReasonData {
    param([object]$Record)

    $score = 0
    $reasons = New-Object 'System.Collections.Generic.List[string]'
    $importSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($import in $Record.Imports) {
        [void]$importSet.Add($import)
    }

    $score += [Math]::Floor($Record.Size / 32)
    $score += ($Record.DirectInternal * 3)
    $score += ($Record.DirectIat * 3)
    $reasons.Add("size:$($Record.Size)") | Out-Null
    $reasons.Add("fanout:$($Record.DirectInternal)/$($Record.DirectIat)") | Out-Null

    if ($importSet.Contains('ntoskrnl.exe!KeQueryUnbiasedInterruptTime')) {
        $score += 32
        $reasons.Add('interrupt-time') | Out-Null
    }

    if ($importSet.Contains('HAL.dll!KeQueryPerformanceCounter')) {
        $score += 24
        $reasons.Add('perf-counter') | Out-Null
    }

    if ($importSet.Contains('ntoskrnl.exe!KeDelayExecutionThread')) {
        $score += 16
        $reasons.Add('delay-thread') | Out-Null
    }

    if ($importSet.Contains('HAL.dll!KeStallExecutionProcessor')) {
        $score += 14
        $reasons.Add('stall-timing') | Out-Null
    }

    foreach ($timerImport in @(
        'ntoskrnl.exe!ExAllocateTimer',
        'ntoskrnl.exe!ExSetTimer',
        'ntoskrnl.exe!ExDeleteTimer',
        'ntoskrnl.exe!KeInitializeEvent',
        'ntoskrnl.exe!KeWaitForSingleObject'
    )) {
        if ($importSet.Contains($timerImport)) {
            $score += 8
            $reasons.Add(($timerImport -split '!')[-1].ToLowerInvariant()) | Out-Null
        }
    }

    $hasSpinEntry = $importSet.Contains('ntoskrnl.exe!KeAcquireSpinLockRaiseToDpc')
    $hasSpinExit = $importSet.Contains('ntoskrnl.exe!KeReleaseSpinLock')
    if ($hasSpinEntry -and $hasSpinExit) {
        $score += 20
        $reasons.Add('spinlock-pair') | Out-Null
    }
    elseif ($hasSpinEntry -or $hasSpinExit) {
        $score += 8
        $reasons.Add('spinlock-single') | Out-Null
    }

    if ($importSet.Contains('ntoskrnl.exe!IoFreeMdl')) {
        $score += 6
        $reasons.Add('mdl') | Out-Null
    }

    if ($importSet.Contains('ntoskrnl.exe!KeGetCurrentIrql')) {
        $score += 4
        $reasons.Add('irql') | Out-Null
    }

    if ($importSet.Contains('ntoskrnl.exe!KeLowerIrql') -or $importSet.Contains('ntoskrnl.exe!KfRaiseIrql')) {
        $score += 4
        $reasons.Add('irql-transition') | Out-Null
    }

    return [pscustomobject]@{
        Score   = $score
        Reasons = @($reasons)
    }
}

$repoRoot = Get-RepoRoot
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath
$targets = @(
    '0x0001B1F0',
    '0x0003FC38',
    '0x0003634C',
    '0x00010D60',
    '0x00015D30',
    '0x000077FC'
)

$records = @(
    foreach ($target in $targets) {
        $record = Load-Record -RepoRoot $repoRoot -Target $target
        $scored = Get-ScoreReasonData -Record $record
        [pscustomobject]@{
            Target         = $record.Target
            Label          = Get-TargetLabel -Target $record.Target
            Function       = $record.Function
            Size           = $record.Size
            DirectInternal = $record.DirectInternal
            DirectIat      = $record.DirectIat
            Imports        = @($record.Imports)
            Score          = $scored.Score
            Reasons        = @($scored.Reasons)
        }
    }
) | Sort-Object -Property Score, Size -Descending

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI Reduced Leaf Shortlist') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Candidate Set' -Body @(
    '- controller family:',
    '  - 0x0001B1F0',
    '  - 0x0003FC38',
    '  - 0x0003634C',
    '- transfer family:',
    '  - 0x00010D60',
    '  - 0x00015D30',
    '  - 0x000077FC'
)

Add-Section -Lines $lines -Title 'Ranked Shortlist' -Body @(
    foreach ($record in $records) {
        $imports = if ($record.Imports.Count -eq 0) { '<none>' } else { $record.Imports -join '; ' }
        $reasons = $record.Reasons -join ', '
        "Target=$($record.Target) | Label=$($record.Label) | Score=$($record.Score) | Function=$($record.Function) | Size=$($record.Size) | Fanout=$($record.DirectInternal)/$($record.DirectIat) | Reasons=$reasons | Imports=$imports"
    }
)

Add-Section -Lines $lines -Title 'Recommendation' -Body @(
    "- primary next study target: $($records[0].Target)",
    "- secondary next study target: $($records[1].Target)",
    '- interpretation:',
    '  - controller-timing bodies still outrank the reduced transfer leaves',
    '  - 0x00015D30 is the strongest remaining transfer-side leaf',
    '  - 0x000077FC stays relevant as a sibling event-side leaf, but it ranks below the reduced controller timing bodies'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
