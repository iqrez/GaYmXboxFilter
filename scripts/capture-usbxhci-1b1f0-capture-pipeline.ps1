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
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-1b1f0-capture-pipeline.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI 1B1F0 Capture Pipeline
Captured: $captured

## Inputs
- out\dev\usbxhci-1b1f0-log-schema.txt
- out\dev\usbxhci-1b1f0-observation-prototype.txt

## Capture Stages
1. probe event emission
   source:
   - ExSetTimer
   - KeWaitForSingleObject enter
   - KeWaitForSingleObject exit
   - KeQueryUnbiasedInterruptTime sample
2. hot-path buffering
   rule:
   - fixed-size append-only event records
   - no string formatting on the probe path
   - drop or truncate with explicit note_flags if buffer pressure appears
3. capture drain
   rule:
   - read out buffered rows after the observation window
   - preserve sequence ordering
   - serialize to a flat row file for offline parsing
4. offline parse
   rule:
   - reconstruct timer_id groupings
   - match arm, wait-enter, and wait-exit rows
   - tolerate unmatched rows explicitly
5. rollup
   rule:
   - produce summary distributions and cadence correlations from the parsed rows

## Recommended Buffer Contract
- record size:
  - fixed-size numeric row
- writer model:
  - single append path with atomic sequence increment
- overflow behavior:
  - do not block
  - mark dropped-row count and set note_flags on subsequent visible rows
- session boundary:
  - explicit capture start
  - explicit capture stop
  - explicit drain after stop

## Recommended Artifacts
- raw row capture:
  - out\dev\usbxhci-1b1f0-events.txt
- parsed correlation view:
  - out\dev\usbxhci-1b1f0-correlated.txt
- aggregate rollup:
  - out\dev\usbxhci-1b1f0-rollup.txt

## Parser Responsibilities
- rebuild timer_id sessions
- compute:
  - arm-to-enter
  - arm-to-exit
  - enter-to-exit
- report:
  - unmatched arms
  - unmatched wakes
  - cross-cpu arms and wakes
  - cadence sample alignment windows

## Safety Constraints
- no synchronous file I/O on probe paths
- no allocation-heavy formatting on probe paths
- no attempt to guarantee perfect pairing in the hot path
- any overflow handling must fail open and preserve system progress

## Practical Conclusion
- the first capture run should be a buffered event stream plus an offline parser, not live pretty-printing
- sequence ordering and cheap append-only writes matter more than perfect real-time interpretation
- this pipeline is now concrete enough to implement without redesigning the observation pass again
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
