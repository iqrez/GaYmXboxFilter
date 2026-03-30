[CmdletBinding()]
param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-transfer-family-assessment.txt'
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
$lines.Add('# USBXHCI Transfer Family Assessment') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Primary Transfer Bodies' -Body @(
    'Helper-heavy pair:',
    '- 0x0001144D',
    '- 0x00011E20',
    'Event-dispatch pair:',
    '- 0x000038CC',
    '- 0x000077FC'
)

Add-Section -Lines $lines -Title 'Helper-Heavy Shared Spine' -Body @(
    '- shared direct callees:',
    '  - 0x00010440',
    '  - 0x000049B4',
    '  - 0x00058B00',
    '- interpretation:',
    '  - 0x00010440 is the only substantive shared body',
    '  - 0x000049B4 is a debug-side leg',
    '  - 0x00058B00 is a thunk',
    '- result:',
    '  - the helper-heavy subfamily reduces to 0x00010440 as the next real body'
)

Add-Section -Lines $lines -Title 'Event-Dispatch Shared Spine' -Body @(
    '- shared direct callees:',
    '  - 0x00003C70',
    '  - 0x00003FA0',
    '  - 0x00004124',
    '  - 0x000049B4',
    '  - 0x00005BC0',
    '- interpretation:',
    '  - 0x00003C70 is a tiny stub',
    '  - 0x00003FA0 is a tiny thunk-like bridge',
    '  - 0x00004124 is a trace leg',
    '  - 0x000049B4 is debug-side context',
    '  - 0x00005BC0 is a small reconnecting bridge',
    '- result:',
    '  - the event-dispatch pair does not expose a better shared deeper body',
    '  - keep 0x000038CC and 0x000077FC as the current event-side leaves'
)

Add-Section -Lines $lines -Title 'Recommendation' -Body @(
    '- continue the helper-heavy line first',
    '- next target: 0x00010440'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
