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
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-1b1f0-log-schema.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI 1B1F0 Log Schema
Captured: $captured

## Inputs
- out\dev\usbxhci-1b1f0-observation-prototype.txt
- out\dev\usbxhci-1b1f0-probe-points.txt

## Event Row Schema
fields:
- sequence
  description: monotonically increasing event id within a single capture session
- probe_point
  description: one of ExSetTimer, KeWaitForSingleObjectEnter, KeWaitForSingleObjectExit, KeQueryUnbiasedInterruptTime
- timestamp_qpc_like
  description: monotonic kernel timestamp used for within-run correlation
- irql
  description: observed IRQL at the probe point
- cpu
  description: processor id if cheaply available
- context_tag
  description: thread or execution-context tag stable enough to relate arm and wake paths
- timer_id
  description: synthetic correlation id assigned at timer-arm time
- due_time
  description: raw due-time value for ExSetTimer rows when available
- period
  description: timer period or recurrence hint for ExSetTimer rows
- wait_status
  description: wait return code for KeWaitForSingleObjectExit rows
- matched_arm_sequence
  description: nearest correlated ExSetTimer sequence for wait rows
- note_flags
  description: bounded bitset for one-shot, periodic, unmatched-wake, cross-cpu, or truncated-context cases

## Minimal Example Rows
sequence=41 probe_point=ExSetTimer timestamp_qpc_like=<t0> irql=<x> cpu=<n> context_tag=<ctx> timer_id=17 due_time=<d> period=<p> note_flags=PERIODIC
sequence=42 probe_point=KeWaitForSingleObjectEnter timestamp_qpc_like=<t1> irql=<x> cpu=<n> context_tag=<ctx> timer_id=17 matched_arm_sequence=41
sequence=43 probe_point=KeWaitForSingleObjectExit timestamp_qpc_like=<t2> irql=<x> cpu=<n> context_tag=<ctx> timer_id=17 matched_arm_sequence=41 wait_status=<s>
sequence=44 probe_point=KeQueryUnbiasedInterruptTime timestamp_qpc_like=<t3> irql=<x> cpu=<n> context_tag=<ctx> timer_id=17 note_flags=POST_WAKE_SAMPLE

## Rollup Buckets
- session summary
  measures:
  - total rows
  - rows per probe point
  - unmatched arms
  - unmatched wakes
- arm-to-enter
  measures:
  - min
  - p50
  - p95
  - max
- arm-to-exit
  measures:
  - min
  - p50
  - p95
  - max
- enter-to-exit
  measures:
  - min
  - p50
  - p95
  - max
- wake clustering
  measures:
  - events per short time bucket
  - cross-cpu dispersion
- cadence correlation
  measures:
  - nearest upper-visible cadence sample window
  - delta between arm-to-exit timing and cadence change

## Logging Constraints
- keep rows append-only and fixed-field so capture parsing stays cheap
- prefer numeric tags over verbose strings on hot paths
- tolerate missing optional fields rather than branching into expensive formatting
- allow unmatched timer_id cases explicitly; do not force perfect pairing in pass 1

## Practical Conclusion
- the first observation pass should emit a compact fixed-field event stream plus an offline rollup
- timer_id, matched_arm_sequence, and context_tag are the critical fields that make arm-to-wake correlation defensible
- the rollup should answer one question first:
  - do ExSetTimer arms and wait wakes align strongly enough to correlate with upper-visible cadence changes
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
