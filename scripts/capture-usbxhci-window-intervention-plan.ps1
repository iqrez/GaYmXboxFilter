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
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-window-intervention-plan.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI Window Intervention Plan
Captured: $captured

## Inputs
- out\dev\usbxhci-cross-family-window-compare.txt
- out\dev\usbxhci-window-modality-matrix.txt

## Ranked Experiment Windows
1. 0x0001B1F0 timer-lifecycle-and-side-context
2. 0x0001B1F0 time-sampling-and-debug-gate
3. 0x0003634C wrapper-and-time-anchor
4. 0x0003634C delayed-sleep-and-trace-side
5. 0x00015D30 transfer-body-and-mdl-turnover

## Recommended Experiment Styles
- 0x0001B1F0 timer-lifecycle-and-side-context
  style: timer-arm and timer-fire skew observation
  rationale: broadest controller timing body with explicit timer lifecycle and wait machinery
  safety bar: no inline multi-millisecond stalls on the hot completion path
- 0x0001B1F0 time-sampling-and-debug-gate
  style: timestamp sampling bias or bounded stall-gate observation
  rationale: cleanest controller window where interrupt-time reads and explicit stall logic coexist
  safety bar: bounded observation only; avoid any sustained stall loop
- 0x0003634C wrapper-and-time-anchor
  style: narrow pacing-anchor contrast
  rationale: best reduced controller window for comparing anchored pacing against the broader 0x0001B1F0 body
  safety bar: preserve wrapper exits and shared-spine flow
- 0x0003634C delayed-sleep-and-trace-side
  style: delayed-thread pacing contrast
  rationale: isolates sleep-based pacing semantics from the larger timer lifecycle
  safety bar: no permanent sleep insertion on an unknown IRQL path
- 0x00015D30 transfer-body-and-mdl-turnover
  style: transfer-side completion and cleanup contrast
  rationale: strongest non-controller window, useful as fallback if controller timing windows prove too brittle
  safety bar: do not hold spinlock or raised IRQL paths artificially

## Unsafe Patterns Already Observed
- aggressive inline parent-path completion delay can trigger watchdog instability
- large fixed stalls on hot paths are not a viable next experiment style
- safe guarded runs only remained stable once the earlier inline-delay design was weakened enough to lose useful control

## Practical Conclusion
- the first credible future experiment is controller-side and timer-centered:
  - 0x0001B1F0 timer-lifecycle-and-side-context
- the second-best controller contrast is:
  - 0x0001B1F0 time-sampling-and-debug-gate
- the transfer side should stay a fallback contrast path, not the primary first attempt
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
