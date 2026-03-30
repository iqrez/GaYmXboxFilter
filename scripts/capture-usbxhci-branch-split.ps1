[CmdletBinding()]
param(
    [string]$HelperMicroMapPath,
    [string]$ClusterProfilePath,
    [uint32[]]$TargetRvas = @(
        [uint32]0x00006E74,
        [uint32]0x00011240,
        [uint32]0x00022E7C
    ),
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-HelperMicroMapPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return (Join-Path (Join-Path (Get-RepoRoot) 'out\dev') 'usbxhci-helper-micromap.txt')
}

function Resolve-ClusterProfilePath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return (Join-Path (Join-Path (Get-RepoRoot) 'out\dev') 'usbxhci-cluster-profile.txt')
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    $defaultDir = Join-Path (Get-RepoRoot) 'out\dev'
    New-Item -ItemType Directory -Force -Path $defaultDir | Out-Null
    return (Join-Path $defaultDir 'usbxhci-branch-split.txt')
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

function Parse-ClusterProfileCandidates {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Cluster profile not found: $Path"
    }

    $candidates = New-Object System.Collections.Generic.List[object]
    $currentFeature = $null
    foreach ($line in Get-Content $Path) {
        if ($line -match '^## (?<feature>.+) Candidate Profiles$') {
            $currentFeature = $matches['feature']
            continue
        }

        if ($line -match '^## ') {
            $currentFeature = $null
            continue
        }

        if (-not $currentFeature) {
            continue
        }

        if ($line -match '^Function=0x(?<begin>[0-9A-Fa-f]+)-0x(?<end>[0-9A-Fa-f]+) \| Section=(?<section>[^|]+) \| RefCount=(?<ref>\d+) \| IatHits=(?<iat>\d+) \| Unwind=0x(?<unwind>[0-9A-Fa-f]+) \| UnwindInfo=(?<unwindInfo>[^|]+) \| Imports=(?<imports>[^|]+) \| Matches=(?<matches>.+)$') {
            $imports = @()
            if ($matches['imports'].Trim() -ne '<none>') {
                $imports = @($matches['imports'].Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
            }

            $candidates.Add([pscustomobject]@{
                Feature    = $currentFeature
                BeginRva   = [uint32]('0x' + $matches['begin'])
                EndRva     = [uint32]('0x' + $matches['end'])
                Section    = $matches['section'].Trim()
                Imports    = $imports
                Matches    = $matches['matches'].Trim()
                IatHits    = [int]$matches['iat']
            }) | Out-Null
        }
    }

    return @($candidates.ToArray())
}

function Find-CandidateByRva {
    param(
        [uint32]$Rva,
        [object[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if ($Rva -ge $candidate.BeginRva -and $Rva -lt $candidate.EndRva) {
            return $candidate
        }
    }

    return $null
}

function Parse-HelperMicroMapTargets {
    param(
        [string]$Path,
        [uint32[]]$TargetRvas
    )

    if (-not (Test-Path $Path)) {
        throw "Helper micro map not found: $Path"
    }

    $targetSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($targetRva in $TargetRvas) {
        [void]$targetSet.Add(('0x{0:X8}' -f $targetRva))
    }

    $targets = New-Object System.Collections.Generic.List[object]
    $current = $null
    $mode = $null

    foreach ($line in Get-Content $Path) {
        if ($line -match '^## Second-Hop Target 0x(?<target>[0-9A-Fa-f]+) from 0x(?<source>[0-9A-Fa-f]+)$') {
            $targetLabel = ('0x{0:X8}' -f [uint32]('0x' + $matches['target']))
            if ($targetSet.Contains($targetLabel)) {
                $current = [pscustomobject]@{
                    BeginRva      = [uint32]('0x' + $matches['target'])
                    SourceHelper  = [uint32]('0x' + $matches['source'])
                    FunctionLabel = $null
                    Section       = $null
                    Size          = 0
                    UnwindInfo    = $null
                    InboundCount  = 0
                    InternalCalls = New-Object System.Collections.Generic.List[object]
                    IatCalls      = New-Object System.Collections.Generic.List[object]
                }
                $targets.Add($current) | Out-Null
            } else {
                $current = $null
            }
            $mode = 'target'
            continue
        }

        if ($line -match '^## Second-Hop Internal 0x(?<target>[0-9A-Fa-f]+)$') {
            $mode = 'internal'
            continue
        }

        if ($line -match '^## Second-Hop IAT 0x(?<target>[0-9A-Fa-f]+)$') {
            $mode = 'iat'
            continue
        }

        if ($line -match '^## ') {
            $mode = $null
            $current = $null
            continue
        }

        if (-not $current) {
            continue
        }

        switch ($mode) {
            'target' {
                if ($line -match '^Function\s+:\s+(?<value>.+)$') {
                    $current.FunctionLabel = $matches['value'].Trim()
                } elseif ($line -match '^Section\s+:\s+(?<value>.+)$') {
                    $current.Section = $matches['value'].Trim()
                } elseif ($line -match '^Size\s+:\s+(?<value>\d+)$') {
                    $current.Size = [int]$matches['value']
                } elseif ($line -match '^UnwindInfo\s+:\s+(?<value>.+)$') {
                    $current.UnwindInfo = $matches['value'].Trim()
                } elseif ($line -match '^InboundFromPrimary\s+:\s+(?<value>\d+)$') {
                    $current.InboundCount = [int]$matches['value']
                }
            }
            'internal' {
                if ($line -match '^Target=0x(?<target>[0-9A-Fa-f]+) \| Calls=(?<count>\d+) \| TargetSection=(?<section>[^|]+) ') {
                    $current.InternalCalls.Add([pscustomobject]@{
                        TargetRva   = [uint32]('0x' + $matches['target'])
                        Count       = [int]$matches['count']
                        TargetSection = $matches['section'].Trim()
                    }) | Out-Null
                }
            }
            'iat' {
                if ($line -match '^Import=(?<import>[^|]+) \| Calls=(?<count>\d+) ') {
                    $current.IatCalls.Add([pscustomobject]@{
                        ImportName = $matches['import'].Trim()
                        Count      = [int]$matches['count']
                    }) | Out-Null
                }
            }
        }
    }

    return @($targets.ToArray())
}

function Get-BranchClassification {
    param(
        [object]$Target,
        [object[]]$Candidates
    )

    $featureHits = New-Object System.Collections.Generic.List[string]
    foreach ($call in $Target.InternalCalls) {
        $candidate = Find-CandidateByRva -Rva $call.TargetRva -Candidates $Candidates
        if ($candidate) {
            $featureHits.Add(('{0}:0x{1:X8}' -f $candidate.Feature, $candidate.BeginRva)) | Out-Null
        }
    }

    $imports = @($Target.IatCalls | ForEach-Object { $_.ImportName })
    $hasBugcheck = $imports -contains 'ntoskrnl.exe!KeBugCheckEx'
    $hasTrace = $imports -contains 'WppRecorder.sys!WppAutoLogTrace'
    $hasSecureSection = $imports -contains 'ntoskrnl.exe!VslDeleteSecureSection'
    $hasSpinlock = ($imports -contains 'ntoskrnl.exe!KeAcquireSpinLockRaiseToDpc') -or ($imports -contains 'ntoskrnl.exe!KeReleaseSpinLock')
    $hasIrql = ($imports -contains 'ntoskrnl.exe!KeGetCurrentIrql') -or ($imports -contains 'ntoskrnl.exe!KfRaiseIrql') -or ($imports -contains 'ntoskrnl.exe!KeLowerIrql')
    $hasWorkItem = $imports -contains 'ntoskrnl.exe!IoQueueWorkItem'
    $controllerHits = @($featureHits | Where-Object { $_ -like 'Controller:*' })

    $classification = 'mixed continuation'
    $rationale = New-Object System.Collections.Generic.List[string]

    if ($controllerHits.Count -gt 0 -or $hasBugcheck -or $hasSecureSection) {
        $classification = 'control/assert drift'
        if ($controllerHits.Count -gt 0) {
            $rationale.Add('reaches controller candidate region(s)') | Out-Null
        }
        if ($hasBugcheck) {
            $rationale.Add('imports KeBugCheckEx') | Out-Null
        }
        if ($hasSecureSection) {
            $rationale.Add('imports VslDeleteSecureSection') | Out-Null
        }
    } elseif ($Target.IatCalls.Count -eq 0 -and $Target.InternalCalls.Count -gt 0) {
        $classification = 'likely hot-path continuation'
        $rationale.Add('stays on internal nonpageable calls with no direct IAT edges in this pass') | Out-Null
    } elseif ($hasTrace -and -not $hasSpinlock -and -not $hasWorkItem -and -not $hasIrql) {
        $classification = 'instrumented wrapper'
        $rationale.Add('primarily collapses into shared thunk plus tracing') | Out-Null
    } elseif ($hasSpinlock -or $hasWorkItem -or $hasIrql) {
        $classification = 'mixed hot/control path'
        if ($hasSpinlock) {
            $rationale.Add('still carries spinlock imports') | Out-Null
        }
        if ($hasWorkItem) {
            $rationale.Add('queues work items') | Out-Null
        }
        if ($hasIrql) {
            $rationale.Add('touches IRQL management') | Out-Null
        }
    }

    return [pscustomobject]@{
        Classification = $classification
        Rationale      = @($rationale.ToArray())
        FeatureHits    = @($featureHits.ToArray() | Select-Object -Unique)
    }
}

$resolvedHelperMicroMapPath = Resolve-HelperMicroMapPath -RequestedPath $HelperMicroMapPath
$resolvedClusterProfilePath = Resolve-ClusterProfilePath -RequestedPath $ClusterProfilePath
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath
$outputDir = Split-Path -Parent $resolvedOutputPath
if ($outputDir) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

$targets = Parse-HelperMicroMapTargets -Path $resolvedHelperMicroMapPath -TargetRvas $TargetRvas
$candidates = Parse-ClusterProfileCandidates -Path $resolvedClusterProfilePath

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Branch Split Assessment') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "HelperMicroMapPath  : $resolvedHelperMicroMapPath",
    "ClusterProfilePath  : $resolvedClusterProfilePath",
    "RequestedTargets    : $([string]::Join(', ', ($TargetRvas | ForEach-Object { '0x{0:X8}' -f $_ })))",
    "ResolvedTargets     : $($targets.Count)",
    "ClusterCandidates   : $($candidates.Count)"
)

foreach ($target in $targets) {
    $classification = Get-BranchClassification -Target $target -Candidates $candidates
    $featureHitLines = if ($classification.FeatureHits.Count -gt 0) { @($classification.FeatureHits) } else { @('<none>') }
    $rationaleLines = if ($classification.Rationale.Count -gt 0) { @($classification.Rationale) } else { @('<none>') }
    $importLines = if ($target.IatCalls.Count -gt 0) {
        @(
            $target.IatCalls |
            Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'ImportName'; Descending = $false } |
            ForEach-Object { "$($_.ImportName) | Calls=$($_.Count)" }
        )
    } else {
        @('No direct IAT calls captured.')
    }
    $internalLines = if ($target.InternalCalls.Count -gt 0) {
        @(
            $target.InternalCalls |
            Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'TargetRva'; Descending = $false } |
            ForEach-Object {
                $candidate = Find-CandidateByRva -Rva $_.TargetRva -Candidates $candidates
                $featureLabel = if ($candidate) { $candidate.Feature } else { '<none>' }
                "Target=0x$('{0:X8}' -f $_.TargetRva) | Calls=$($_.Count) | TargetSection=$($_.TargetSection) | FeatureHit=$featureLabel"
            }
        )
    } else {
        @('No internal targets captured.')
    }

    Add-Section -Lines $lines -Title ("Target 0x{0:X8}" -f $target.BeginRva) -Body @(
        "SourceHelper       : 0x$('{0:X8}' -f $target.SourceHelper)",
        "Function           : $($target.FunctionLabel)",
        "Section            : $($target.Section)",
        "Size               : $($target.Size)",
        "UnwindInfo         : $($target.UnwindInfo)",
        "InboundFromPrimary : $($target.InboundCount)",
        "Classification     : $($classification.Classification)"
    )

    Add-Section -Lines $lines -Title ("Rationale 0x{0:X8}" -f $target.BeginRva) -Body $rationaleLines
    Add-Section -Lines $lines -Title ("Feature Hits 0x{0:X8}" -f $target.BeginRva) -Body $featureHitLines
    Add-Section -Lines $lines -Title ("Internal Targets 0x{0:X8}" -f $target.BeginRva) -Body $internalLines
    Add-Section -Lines $lines -Title ("Imports 0x{0:X8}" -f $target.BeginRva) -Body $importLines
}

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It consumes the helper micro-map plus the current cluster profile and classifies three second-hop targets.',
    'The goal is not to prove semantics; it is to split likely hot-path continuation from wrapper and control/assert drift before any deeper offline reverse engineering.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "USBXHCI branch split assessment written to $resolvedOutputPath"
