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
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-window-modality-matrix.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI Window Modality Matrix
Captured: $captured

## Inputs
- out\dev\usbxhci-controller-window-compare.txt
- out\dev\usbxhci-15d30-window-map.txt
- out\dev\usbxhci-cross-family-window-compare.txt

## Ranked Windows By Timing Modality
1. 0x0001B1F0 timer-lifecycle-and-side-context
   modalities:
   - timer allocation and teardown
   - timer arming
   - event wait and wake coordination
   - controller-side timing orchestration
2. 0x0001B1F0 time-sampling-and-debug-gate
   modalities:
   - interrupt-time sampling
   - explicit stall timing
   - gated controller pacing
3. 0x0003634C wrapper-and-time-anchor
   modalities:
   - interrupt-time anchored pacing
   - narrow wrapper-mediated timing contrast
4. 0x0003634C delayed-sleep-and-trace-side
   modalities:
   - delayed-thread pacing
   - trace-side pacing contrast
5. 0x00015D30 transfer-body-and-mdl-turnover
   modalities:
   - transfer event handling
   - MDL turnover
   - transfer-body timing contrast
6. 0x00015D30 spinlock-entry-and-irql-raise
   modalities:
   - spinlock ownership
   - IRQL raise and lower transitions
   - hot transfer entry timing

## Modality Grouping
- timer and wait orchestration:
  - 0x0001B1F0 timer-lifecycle-and-side-context
- time-sampling and stall gating:
  - 0x0001B1F0 time-sampling-and-debug-gate
- anchored controller pacing:
  - 0x0003634C wrapper-and-time-anchor
- delayed-thread pacing:
  - 0x0003634C delayed-sleep-and-trace-side
- transfer-body semantics:
  - 0x00015D30 transfer-body-and-mdl-turnover
- lock and IRQL semantics:
  - 0x00015D30 spinlock-entry-and-irql-raise

## Practical Conclusion
- if a future host-side experiment wants the broadest controller leverage, start with:
  - 0x0001B1F0 timer-lifecycle-and-side-context
- if it wants the narrowest controller pacing contrast, start with:
  - 0x0003634C wrapper-and-time-anchor
- if it wants the cleanest transfer-side contrast, start with:
  - 0x00015D30 transfer-body-and-mdl-turnover
- this matrix is the first point where the spike is partitioned by timing modality instead of only by body or window rank
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
