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
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-1b1f0-observation-prototype.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI 1B1F0 Observation Prototype
Captured: $captured

## Inputs
- out\dev\usbxhci-1b1f0-experiment-spec.txt
- out\dev\usbxhci-1b1f0-probe-points.txt

## Scope
- body: 0x0001B1F0
- primary window: timer-lifecycle-and-side-context
- probe points:
  - ExSetTimer
  - KeWaitForSingleObject
- secondary comparison point:
  - KeQueryUnbiasedInterruptTime

## Prototype Goal
- build the first host-side experiment as observation-first
- capture timer-arm to wait-wake relationships without inserting a new hot-path delay
- prove whether the controller timing core exposes a measurable propagation path before any broader perturbation attempt

## Phase Order
1. ExSetTimer observation
   capture:
   - arm timestamp
   - due-time parameters
   - recurrence or one-shot classification
2. KeWaitForSingleObject observation
   capture:
   - wait entry timestamp
   - wait return timestamp
   - wait status
   - nearest prior timer arm correlation
3. KeQueryUnbiasedInterruptTime comparison
   capture:
   - timestamp sample around arm and wake boundaries
   - drift or skew against the arm-to-wake path

## Output Shape
- per-event log rows should be sufficient
- minimum row fields:
  - sequence
  - probe point
  - timestamp
  - irql
  - thread or execution context tag
  - timer correlation id
  - wait status if present
- aggregate rollup:
  - arm-to-wake delta distribution
  - wake clustering
  - correlation against upper-visible cadence samples

## Success Criteria
- no watchdog or stability regression
- repeated timer-arm to wait-wake chains are observable
- arm-to-wake timing can be correlated with upper-visible cadence or counter changes

## Abort Conditions
- any watchdog or livelock symptom
- persistent controller loss or enumeration instability
- probe output volume so high that it becomes the dominant disturbance

## Explicit Non-Goals For Pass 1
- no broad body-wide perturbation
- no multi-millisecond inline stall
- no sleep insertion at uncertain IRQL
- no transfer-side fallback work during this pass

## Practical Conclusion
- the first live host-side prototype should be a correlation pass, not a control pass
- ExSetTimer and KeWaitForSingleObject are the first safe enough anchors to prove whether 0x0001B1F0 gives a usable timing signal
- only after that correlation succeeds should the branch consider any bounded bias experiment
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
