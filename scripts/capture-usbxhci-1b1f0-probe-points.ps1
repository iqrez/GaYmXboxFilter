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
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-1b1f0-probe-points.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI 1B1F0 Probe Points
Captured: $captured

## Inputs
- out\dev\usbxhci-1b1f0-window-map.txt
- out\dev\usbxhci-1b1f0-experiment-spec.txt

## Primary Window
- 0x0001B1F0 timer-lifecycle-and-side-context

### Probe Candidates
1. ExAllocateTimer
   mode:
   - observe-only
   - allocation/failure correlation
   why: lowest-risk timer lifecycle anchor in the primary window
2. ExSetTimer
   mode:
   - observe-only
   - bounded timer-arm skew measurement
   why: strongest timer-arm point without touching hot completion timing directly
3. KeWaitForSingleObject
   mode:
   - observe-only
   - wait-duration correlation
   why: best place to measure whether timer-side pacing propagates into controller waits
4. KeInitializeEvent
   mode:
   - observe-only
   why: event setup context only; useful for sequencing, not direct timing control
5. KeGetCurrentIrql
   mode:
   - observe-only
   why: safety and path-classification probe, not a timing lever

## Secondary Window
- 0x0001B1F0 time-sampling-and-debug-gate

### Probe Candidates
1. KeQueryUnbiasedInterruptTime
   mode:
   - observe-only
   - bounded timestamp-bias experiment
   why: cleanest time anchor in the secondary window
2. KeStallExecutionProcessor
   mode:
   - observe-only preferred
   - bounded micro-tail comparison only
   why: direct timing primitive, but closest to the earlier unsafe pattern
3. 0x0000BF40 / 0x0000C970 side gates
   mode:
   - observe-only
   why: classification and branch-correlation points, not first timing levers

## Do-Not-Start-With Points
- broad body-wide perturbation across all 0x0001B1F0 windows
- any multi-millisecond stall rooted at KeStallExecutionProcessor
- any sleep insertion whose IRQL safety is not already proven
- direct reuse of the parent-path completion-delay mechanism

## Preferred First Live Order
1. ExSetTimer observation and arm-to-fire correlation
2. KeWaitForSingleObject observation and wake correlation
3. KeQueryUnbiasedInterruptTime bounded timestamp comparison
4. only then consider any tightly bounded KeStallExecutionProcessor contrast

## Practical Conclusion
- the first real host-side probe should attach to ExSetTimer and KeWaitForSingleObject inside 0x0001B1F0
- KeQueryUnbiasedInterruptTime is the safest secondary timing contrast
- KeStallExecutionProcessor remains a late-stage contrast point, not the first live lever
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
