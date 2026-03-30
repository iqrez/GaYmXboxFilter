[CmdletBinding()]
param(
    [string]$FeatureMapPath,
    [string[]]$WalkPaths,
    [string]$OutputPath,
    [int]$TopCount = 25
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-FeatureMapPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-feature-map.txt'
}

function Resolve-WalkPaths {
    param([string[]]$RequestedPaths)

    if ($RequestedPaths -and $RequestedPaths.Count -gt 0) {
        return $RequestedPaths
    }

    $defaultPaths = @(
        (Join-Path (Get-RepoRoot) 'out\dev\usbxhci-exhaustive-walk.txt'),
        (Join-Path (Get-RepoRoot) 'out\dev\usbxhci-feature-closure-walk.txt'),
        (Join-Path (Get-RepoRoot) 'out\dev\usbxhci-runtime-closure-walk.txt')
    )

    return @($defaultPaths | Where-Object { Test-Path -LiteralPath $_ })
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-intervention-shortlist.txt'
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

function Get-FeatureCategories {
    param([string]$Path)

    $map = @{}
    $currentCategory = $null

    foreach ($line in Get-Content -Path $Path) {
        if ($line -match '^## (Controller|Endpoint|Ring|Transfer|Interrupter) Candidate Functions$') {
            $currentCategory = $matches[1]
            continue
        }

        if ($currentCategory -and $line -match '^Function=(0x[0-9A-F]+)-') {
            $map[$matches[1]] = $currentCategory
        }
    }

    return $map
}

function Get-TargetsFromWalks {
    param([string[]]$Paths)

    $targets = @{}

    foreach ($path in $Paths) {
        $lines = Get-Content -Path $path
        $current = $null

        foreach ($line in $lines) {
            if ($line -match '^## Target (0x[0-9A-F]+)$') {
                if ($null -ne $current -and -not $targets.ContainsKey($current.Target)) {
                    $targets[$current.Target] = $current
                }

                $current = [ordered]@{
                    Target         = $matches[1]
                    Class          = ''
                    Size           = 0
                    DirectInternal = 0
                    DirectIat      = 0
                    Imports        = New-Object 'System.Collections.Generic.List[string]'
                }
                continue
            }

            if ($null -eq $current) {
                continue
            }

            if ($line -match '^Class\s+:\s+(.+)$') {
                $current.Class = $matches[1].Trim()
                continue
            }

            if ($line -match '^Size\s+:\s+(\d+)$') {
                $current.Size = [int]$matches[1]
                continue
            }

            if ($line -match '^DirectInternal\s+:\s+(\d+)$') {
                $current.DirectInternal = [int]$matches[1]
                continue
            }

            if ($line -match '^DirectIat\s+:\s+(\d+)$') {
                $current.DirectIat = [int]$matches[1]
                continue
            }

            if ($line -match '^\s+Import=([^|]+)\s+\|') {
                $importName = $matches[1].Trim()
                if (-not $current.Imports.Contains($importName)) {
                    $current.Imports.Add($importName) | Out-Null
                }
            }
        }

        if ($null -ne $current -and -not $targets.ContainsKey($current.Target)) {
            $targets[$current.Target] = $current
        }
    }

    return @($targets.Values | ForEach-Object { [pscustomobject]$_ })
}

function Get-ClassScore {
    param([string]$Class)

    switch ($Class) {
        'substantive' { return 45 }
        'mixed' { return 22 }
        'bridge' { return 8 }
        'opaque' { return 6 }
        'stub' { return -18 }
        'thunk' { return -25 }
        'trace' { return -18 }
        'etw' { return -22 }
        'path-string' { return -22 }
        default { return 0 }
    }
}

function Get-CategoryScore {
    param([string]$Category)

    switch ($Category) {
        'Transfer' { return 26 }
        'Endpoint' { return 20 }
        'Interrupter' { return 18 }
        'Controller' { return 14 }
        'Ring' { return 6 }
        default { return 0 }
    }
}

function Get-ImportScore {
    param([string[]]$Imports)

    $score = 0
    $reasons = New-Object 'System.Collections.Generic.List[string]'

    foreach ($importName in $Imports) {
        switch -Regex ($importName) {
            'KeAcquireSpinLockRaiseToDpc' { $score += 18; $reasons.Add('spinlock-entry') | Out-Null; continue }
            'KeReleaseSpinLock' { $score += 18; $reasons.Add('spinlock-exit') | Out-Null; continue }
            'KeQueryUnbiasedInterruptTime' { $score += 18; $reasons.Add('interrupt-time') | Out-Null; continue }
            'KeStallExecutionProcessor' { $score += 16; $reasons.Add('stall-timing') | Out-Null; continue }
            'KeDelayExecutionThread' { $score += 14; $reasons.Add('delay-thread') | Out-Null; continue }
            'ExAllocateTimer' { $score += 14; $reasons.Add('timer-alloc') | Out-Null; continue }
            'ExSetTimer' { $score += 14; $reasons.Add('timer-set') | Out-Null; continue }
            'ExDeleteTimer' { $score += 12; $reasons.Add('timer-delete') | Out-Null; continue }
            'KeWaitForSingleObject' { $score += 10; $reasons.Add('wait') | Out-Null; continue }
            'KeGetCurrentIrql' { $score += 8; $reasons.Add('irql') | Out-Null; continue }
            'IoSetCompletionRoutineEx' { $score += 10; $reasons.Add('completion-hook') | Out-Null; continue }
            'IoReuseIrp' { $score += 8; $reasons.Add('irp-reuse') | Out-Null; continue }
            'IoQueueWorkItem' { $score += 8; $reasons.Add('work-item') | Out-Null; continue }
            'ExAllocatePool2' { $score += 3; $reasons.Add('alloc') | Out-Null; continue }
            'WppAutoLogTrace' { $score -= 18; $reasons.Add('trace-penalty') | Out-Null; continue }
            'DbgPrintEx|DbgPrint' { $score -= 14; $reasons.Add('debug-penalty') | Out-Null; continue }
            'KdRefreshDebuggerNotPresent' { $score -= 12; $reasons.Add('debug-check-penalty') | Out-Null; continue }
            '^Etw' { $score -= 18; $reasons.Add('etw-penalty') | Out-Null; continue }
            'IoQueryFullDriverPath|Rtl.*String' { $score -= 18; $reasons.Add('path-string-penalty') | Out-Null; continue }
        }
    }

    return [pscustomobject]@{
        Score   = $score
        Reasons = @($reasons | Select-Object -Unique)
    }
}

function Score-Target {
    param(
        [object]$Target,
        [hashtable]$FeatureCategories
    )

    $category = if ($FeatureCategories.ContainsKey($Target.Target)) { $FeatureCategories[$Target.Target] } else { 'Unmapped' }
    $importInfo = Get-ImportScore -Imports $Target.Imports

    $sizeScore = [Math]::Min([Math]::Floor($Target.Size / 32), 20)
    $fanOutScore = ([Math]::Min($Target.DirectInternal, 12) * 3) + ([Math]::Min($Target.DirectIat, 6) * 4)
    $classScore = Get-ClassScore -Class $Target.Class
    $categoryScore = Get-CategoryScore -Category $category
    $total = $classScore + $categoryScore + $sizeScore + $fanOutScore + $importInfo.Score

    $reasonTags = New-Object 'System.Collections.Generic.List[string]'
    foreach ($tag in @(
        "class:$($Target.Class)",
        "category:$category",
        "size:$($Target.Size)",
        "fanout:$($Target.DirectInternal)/$($Target.DirectIat)"
    ) + $importInfo.Reasons) {
        if (-not $reasonTags.Contains($tag)) {
            $reasonTags.Add($tag) | Out-Null
        }
    }

    return [pscustomobject]@{
        Target         = $Target.Target
        Category       = $category
        Class          = $Target.Class
        Size           = $Target.Size
        DirectInternal = $Target.DirectInternal
        DirectIat      = $Target.DirectIat
        Imports        = @($Target.Imports)
        Score          = [int]$total
        ReasonTags     = @($reasonTags)
    }
}

$featureMapPath = Resolve-FeatureMapPath -RequestedPath $FeatureMapPath
$walkPaths = Resolve-WalkPaths -RequestedPaths $WalkPaths
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath

foreach ($path in @($featureMapPath) + $walkPaths) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required file not found: $path"
    }
}

$outputDirectory = Split-Path -Parent $outputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$featureCategories = Get-FeatureCategories -Path $featureMapPath
$targets = Get-TargetsFromWalks -Paths $walkPaths
$ranked = @(
    $targets |
        ForEach-Object { Score-Target -Target $_ -FeatureCategories $featureCategories } |
        Sort-Object -Property @(
            @{ Expression = 'Score'; Descending = $true },
            @{ Expression = 'Target'; Descending = $false }
        )
)
$topTargets = @($ranked | Select-Object -First $TopCount)

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI Intervention Shortlist') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "WalkPaths    : $($walkPaths.Count)",
    "ParsedTargets: $($targets.Count)",
    "TopCount     : $TopCount"
)

Add-Section -Lines $lines -Title 'Top Candidates' -Body @(
    foreach ($target in $topTargets) {
        $importSummary = if ($target.Imports.Count -eq 0) { '<none>' } else { ($target.Imports -join '; ') }
        $reasonSummary = $target.ReasonTags -join ', '
        "Target=$($target.Target) | Score=$($target.Score) | Category=$($target.Category) | Class=$($target.Class) | Size=$($target.Size) | Fanout=$($target.DirectInternal)/$($target.DirectIat) | Reasons=$reasonSummary | Imports=$importSummary"
    }
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
