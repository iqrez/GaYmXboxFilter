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
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-1b1f0-parser-rollup-spec.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI 1B1F0 Parser And Rollup Spec
Captured: $captured

## Inputs
- out\dev\usbxhci-1b1f0-log-schema.txt
- out\dev\usbxhci-1b1f0-capture-pipeline.txt

## Parser Pass Order
1. load raw rows
   rules:
   - preserve sequence ordering
   - reject malformed rows into a side count, not a hard stop
2. normalize row types
   rules:
   - map probe_point into enum form
   - normalize note_flags into bitset form
   - keep missing optional fields as null, not zero
3. build arm table
   rules:
   - key first by timer_id
   - keep per-timer ordered arm rows
   - retain context_tag and cpu for later tie-breaking
4. match wait-enter and wait-exit rows
   rules:
   - first try exact timer_id match
   - then nearest prior arm on same context_tag
   - then nearest prior arm on same cpu
   - if still unmatched, keep explicit unmatched classification
5. construct correlated chains
   output form:
   - arm
   - optional wait-enter
   - optional wait-exit
   - optional nearby unbiased-time sample
6. compute rollups
   rules:
   - summarize only by completed chain class first
   - report unmatched buckets separately

## Matching Confidence Tiers
- high
  criteria:
  - exact timer_id match
  - same context_tag
- medium
  criteria:
  - exact timer_id match
  - cross-context but near in sequence or timestamp
- low
  criteria:
  - nearest prior arm by context_tag or cpu only
- unmatched
  criteria:
  - no defensible arm candidate

## Derived Measures
- arm_to_enter_delta
  formula:
  - wait_enter.timestamp_qpc_like - arm.timestamp_qpc_like
- arm_to_exit_delta
  formula:
  - wait_exit.timestamp_qpc_like - arm.timestamp_qpc_like
- enter_to_exit_delta
  formula:
  - wait_exit.timestamp_qpc_like - wait_enter.timestamp_qpc_like
- sample_to_exit_delta
  formula:
  - unbiased_time_sample.timestamp_qpc_like - wait_exit.timestamp_qpc_like

## Rollup Sections
1. session summary
   fields:
   - total_rows
   - parsed_rows
   - malformed_rows
   - total_arms
   - completed_chains
   - unmatched_arms
   - unmatched_wait_enters
   - unmatched_wait_exits
2. confidence summary
   fields:
   - high_count
   - medium_count
   - low_count
   - unmatched_count
3. latency distributions
   fields:
   - arm_to_enter min p50 p95 max
   - arm_to_exit min p50 p95 max
   - enter_to_exit min p50 p95 max
4. clustering summary
   fields:
   - wakes_per_bucket
   - max_bucket_density
   - cross_cpu_chain_count
5. cadence correlation
   fields:
   - nearest cadence sample id
   - cadence delta before and after chain
   - strongest observed correlation bucket

## Output Files
- correlated chains:
  - out\dev\usbxhci-1b1f0-correlated.txt
- aggregate rollup:
  - out\dev\usbxhci-1b1f0-rollup.txt
- optional parser diagnostics:
  - out\dev\usbxhci-1b1f0-parser-diagnostics.txt

## Practical Conclusion
- the parser should favor explicit confidence scoring over pretending all matches are equally good
- unmatched rows are signal, not noise, and must remain visible in the rollup
- the first question the rollup must answer is whether high-confidence arm-to-wake chains line up tightly enough to compare against upper-visible cadence changes
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
