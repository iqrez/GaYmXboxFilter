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
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-cross-family-window-compare.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI Cross-Family Window Compare
Captured: $captured

## Inputs
- primary controller window map: out\dev\usbxhci-1b1f0-window-map.txt
- secondary controller window map: out\dev\usbxhci-3634c-window-map.txt
- transfer leaf window map: out\dev\usbxhci-15d30-window-map.txt

## Ranked Window Order
1. 0x0001B1F0 timer-lifecycle-and-side-context
   role: richest controller-side timing orchestration window with timers, waits, and side-context setup in one bounded region
2. 0x0001B1F0 time-sampling-and-debug-gate
   role: highest-value controller window where interrupt-time sampling and explicit stall timing coexist
3. 0x0003634C wrapper-and-time-anchor
   role: narrow controller pacing contrast window centered on interrupt-time anchored pacing
4. 0x0003634C delayed-sleep-and-trace-side
   role: narrow delayed-thread pacing contrast window without the broader 0x0001B1F0 timer lifecycle
5. 0x00015D30 transfer-body-and-mdl-turnover
   role: strongest transfer-side event window with real body semantics and MDL turnover still visible
6. 0x00015D30 spinlock-entry-and-irql-raise
   role: cleanest transfer-side lock and IRQL timing window
7. 0x0001B1F0 final-handoff
   role: boundary window into inner timing descendant 0x0003FC38
8. 0x0003634C irql-and-shared-spine
   role: useful shared controller pacing context, but less isolated than the higher-ranked controller windows
9. 0x00015D30 trace-side-and-cleanup-exits
   role: transfer-side contrast window for trace churn and MDL cleanup exits
10. 0x0003634C terminal-side-bridges
    role: lower-value controller exit window dominated by bridge and interrupt-side context
11. 0x0001B1F0 setup-and-bridge
    role: lower-value primary controller setup window dominated by wrapper and alternate-bridge setup
12. 0x00015D30 thunk-terminal-edge
    role: lowest-value transfer window because it drains only into 0x00058B00

## Family Readout
- highest-value controller windows:
  - 0x0001B1F0 timer-lifecycle-and-side-context
  - 0x0001B1F0 time-sampling-and-debug-gate
  - 0x0003634C wrapper-and-time-anchor
  - 0x0003634C delayed-sleep-and-trace-side
- highest-value transfer windows:
  - 0x00015D30 transfer-body-and-mdl-turnover
  - 0x00015D30 spinlock-entry-and-irql-raise

## Practical Conclusion
- if any future host-side experiment starts at the window level, begin on the controller family, not the transfer family
- use the transfer windows as contrast and fallback study targets after controller windows are exhausted
- the first cross-family intervention order is:
  - 0x0001B1F0 timer-lifecycle-and-side-context
  - 0x0001B1F0 time-sampling-and-debug-gate
  - 0x0003634C wrapper-and-time-anchor
  - 0x0003634C delayed-sleep-and-trace-side
  - 0x00015D30 transfer-body-and-mdl-turnover
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
