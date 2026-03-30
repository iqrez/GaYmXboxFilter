[CmdletBinding()]
param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    $scriptRootParent = Split-Path -Parent $PSScriptRoot
    if (Test-Path -LiteralPath (Join-Path $scriptRootParent '.git')) {
        return $scriptRootParent
    }

    $currentLocation = (Get-Location).Path
    if (Test-Path -LiteralPath (Join-Path $currentLocation '.git')) {
        return $currentLocation
    }

    return $scriptRootParent
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-final-intervention-summary.txt'
}

function Add-Section {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Title,
        [string[]]$Body
    )

    $Lines.Add("## $Title") | Out-Null
    foreach ($line in $Body) {
        $Lines.Add($line) | Out-Null
    }
    $Lines.Add('') | Out-Null
}

$outputPath = Resolve-OutputPath -RequestedPath $OutputPath

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI Final Intervention Summary') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Surviving Study Targets' -Body @(
    '1. 0x0001B1F0',
    '   role: primary controller timing core',
    '   why it matters: broadest timing/orchestration surface in the spike, including interrupt-time, explicit stall timing, timer allocation/set/delete, event setup, and wait',
    '2. 0x0003634C',
    '   role: secondary controller timing core',
    '   why it matters: narrower pacing-oriented contrast body with interrupt-time, delayed-thread sleep, and IRQL awareness but without the wider timer/wait orchestration of 0x0001B1F0',
    '3. 0x00015D30',
    '   role: primary transfer-side event leaf',
    '   why it matters: strongest remaining transfer-side leaf, preserving spinlock, MDL, and IRQL transition machinery after the transfer-family reductions'
)

Add-Section -Lines $lines -Title 'Supporting Context' -Body @(
    '- 0x0003FC38 remains the only meaningful direct timing descendant under 0x0001B1F0',
    '- 0x000077FC remains the sibling event-side transfer leaf, but it ranks below 0x00015D30 as a deeper study target',
    '- 0x0000D210 is still only the alternate bridge path, not a competing timing core'
)

Add-Section -Lines $lines -Title 'Recommended Order' -Body @(
    '1. start with 0x0001B1F0',
    '   reason: best candidate for host-side timing/orchestration behavior that could plausibly affect controller-visible cadence',
    '2. use 0x0003634C as the controller contrast body',
    '   reason: lets us separate broad timer/wait orchestration from narrower pacing-style timing',
    '3. use 0x00015D30 as the transfer-side contrast body',
    '   reason: best surviving transfer leaf when comparing controller timing work against transfer-event-side behavior'
)

Add-Section -Lines $lines -Title 'Practical Branch Conclusion' -Body @(
    '- if deeper host-stack study continues, the branch should stop broad graph walking and concentrate on these three targets',
    '- controller-first work should treat 0x0001B1F0 as the primary body, 0x0003FC38 as its inner timing helper, and 0x0003634C as the contrast body',
    '- transfer-side work should treat 0x00015D30 as the only remaining top-tier leaf and keep 0x000077FC only as secondary context'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
