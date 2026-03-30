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

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-controller-window-compare.txt'
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
$lines.Add('# USBXHCI Controller Window Compare') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Inputs' -Body @(
    '- primary body window map: out\dev\usbxhci-1b1f0-window-map.txt',
    '- secondary body window map: out\dev\usbxhci-3634c-window-map.txt'
)

Add-Section -Lines $lines -Title 'Primary Controller Windows' -Body @(
    '0x0001B1F0 windows:',
    '- time-sampling-and-debug-gate',
    '- timer-lifecycle-and-side-context',
    '- final-handoff',
    '- setup-and-bridge (lower value)'
)

Add-Section -Lines $lines -Title 'Secondary Controller Windows' -Body @(
    '0x0003634C windows:',
    '- irql-and-shared-spine',
    '- wrapper-and-time-anchor',
    '- delayed-sleep-and-trace-side',
    '- terminal-side-bridges (lower value)'
)

Add-Section -Lines $lines -Title 'Pairing' -Body @(
    '- 0x0001B1F0 time-sampling-and-debug-gate <-> 0x0003634C wrapper-and-time-anchor',
    '  comparison role: interrupt-time anchored pacing window versus interrupt-time plus explicit stall gate',
    '- 0x0001B1F0 timer-lifecycle-and-side-context <-> 0x0003634C delayed-sleep-and-trace-side',
    '  comparison role: timer allocation/setup/wait flow versus delayed-thread pacing flow',
    '- 0x0001B1F0 final-handoff <-> 0x0003634C terminal-side-bridges',
    '  comparison role: direct timing-descendant handoff and timer teardown versus bridge-to-stub exit context'
)

Add-Section -Lines $lines -Title 'Intervention Order' -Body @(
    '1. 0x0001B1F0 timer-lifecycle-and-side-context',
    '   why: richest controller-side timing orchestration window in the spike',
    '2. 0x0001B1F0 time-sampling-and-debug-gate',
    '   why: cleanest place where interrupt-time reads and explicit stall timing coexist',
    '3. 0x0003634C wrapper-and-time-anchor',
    '   why: best contrast window for interrupt-time pacing without the broader timer lifecycle',
    '4. 0x0003634C delayed-sleep-and-trace-side',
    '   why: best contrast window for delayed-thread pacing behavior',
    '5. 0x0001B1F0 final-handoff',
    '   why: direct boundary to the inner timing descendant 0x0003FC38',
    '6. 0x0003634C irql-and-shared-spine',
    '   why: shared-spine context remains useful, but less isolated as a timing window than the anchors above',
    '7. 0x0003634C terminal-side-bridges',
    '   why: lowest-value controller window because it mostly exits into bridge and interrupt-side context',
    '8. 0x0001B1F0 setup-and-bridge',
    '   why: lowest-value primary window because it is dominated by wrapper and bridge setup'
)

Add-Section -Lines $lines -Title 'Practical Conclusion' -Body @(
    '- if any future host-side experiment starts from controller windows rather than whole bodies, begin with the two primary 0x0001B1F0 windows:',
    '  - timer-lifecycle-and-side-context',
    '  - time-sampling-and-debug-gate',
    '- use the two mid-ranked 0x0003634C windows as the narrow pacing contrast set:',
    '  - wrapper-and-time-anchor',
    '  - delayed-sleep-and-trace-side'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
