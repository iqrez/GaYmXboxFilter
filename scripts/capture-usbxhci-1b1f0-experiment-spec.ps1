param(
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $scriptDir = Split-Path -Parent $PSCommandPath
    $candidate = Resolve-Path (Join-Path $scriptDir "..")
    if (Test-Path (Join-Path $candidate ".git")) {
        return $candidate
    }

    return (Get-Location).Path
}

$repoRoot = Get-RepoRoot

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-1b1f0-experiment-spec.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI 1B1F0 Experiment Spec
Captured: $captured

## Inputs
- out\dev\usbxhci-1b1f0-window-map.txt
- out\dev\usbxhci-window-intervention-plan.txt

## Target
- body: 0x0001B1F0
- primary window: timer-lifecycle-and-side-context
- secondary window: time-sampling-and-debug-gate

## Experiment Goal
- study host-side timing influence at the controller core without repeating the earlier unsafe inline completion-delay design
- keep the first intervention bounded to observation and narrow bias, not brute-force delay

## Preferred Prototype Order
1. Timer-arm and timer-fire observation around:
   - ExAllocateTimer
   - ExSetTimer
   - KeWaitForSingleObject
2. Narrow timestamp-bias or gate-observation around:
   - KeQueryUnbiasedInterruptTime
   - KeStallExecutionProcessor
3. Final-handoff correlation only after the first two are characterized:
   - 0x0003FC38
   - ExDeleteTimer

## Safe Boundaries
- do not insert multi-millisecond inline stalls on a hot completion path
- do not hold a raised IRQL path open artificially
- do not reuse the previous parent-path completion-delay mechanism as the starting point
- prefer one bounded observation point per pass instead of whole-body perturbation

## Unsafe Starting Patterns
- sustained inline stall loops
- completion-path sleep insertion on unknown IRQL
- broad body-wide perturbation across all 0x0001B1F0 windows at once

## First Experiment Shape
- mode: read-mostly or minimally perturbative
- scope:
  - timer-lifecycle-and-side-context only
- first success condition:
  - prove that bounded observation or tiny bias at this window changes upper-visible cadence or timing counters without destabilizing the machine
- failure condition:
  - no observable effect under a bounded safe intervention
  - or any sign of watchdog / livelock / persistent controller instability

## Fallback Order
1. 0x0001B1F0 time-sampling-and-debug-gate
2. 0x0003634C wrapper-and-time-anchor
3. 0x00015D30 transfer-body-and-mdl-turnover

## Practical Conclusion
- the first real host-side experiment should start at 0x0001B1F0 timer-lifecycle-and-side-context
- the experiment should be narrow, timer-centered, and explicitly hostile to long inline delays
- if that window proves too brittle, fall back to 0x0001B1F0 time-sampling-and-debug-gate before leaving the controller family
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
