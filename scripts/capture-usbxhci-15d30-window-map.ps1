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
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-15d30-window-map.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI 15D30 Window Map
Captured: $captured

## Source
- body micro-map: out\dev\usbxhci-15d30-micromap.txt

## Bounded Windows
0x00015D30 windows:
- spinlock-entry-and-irql-raise
- transfer-body-and-mdl-turnover
- trace-side-and-cleanup-exits
- thunk-terminal-edge (lower value)

## Window Roles
- spinlock-entry-and-irql-raise
  role: initial hot transfer entry where spinlock ownership and IRQL transition begin
- transfer-body-and-mdl-turnover
  role: core transfer-side work window where MDL release and steady-state transfer handling remain closest to the live event path
- trace-side-and-cleanup-exits
  role: side exits into trace legs and MDL cleanup descendants:
    - 0x0002F368
    - 0x00016220
    - 0x000163D8
- thunk-terminal-edge
  role: terminal handoff into 0x00058B00 only

## Intervention Order
1. 0x00015D30 transfer-body-and-mdl-turnover
   why: strongest remaining transfer-side window with real event-path semantics and cleanup pressure still in view
2. 0x00015D30 spinlock-entry-and-irql-raise
   why: cleanest transfer-side window for studying lock/IRQL timing without broader controller timer machinery
3. 0x00015D30 trace-side-and-cleanup-exits
   why: useful to separate trace churn and MDL-side cleanup from the hotter transfer body
4. 0x00015D30 thunk-terminal-edge
   why: lowest-value transfer window because it only drains into the tiny shared thunk 0x00058B00

## Practical Conclusion
- if transfer-side study continues, start with:
  - transfer-body-and-mdl-turnover
  - spinlock-entry-and-irql-raise
- keep trace-side-and-cleanup-exits only as contrast context
- treat thunk-terminal-edge as terminal noise rather than a primary intervention window
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
