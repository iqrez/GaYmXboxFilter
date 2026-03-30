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

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-3634c-window-map.txt'
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

function Parse-WindowEntries {
    param([string]$Path)

    $entries = New-Object 'System.Collections.Generic.List[object]'
    foreach ($line in Get-Content -Path $Path) {
        if ($line -match '^Target=(0x[0-9A-F]+) \| Calls=(\d+) \| TargetSection=([^|]+) \| SampleSite=(0x[0-9A-F]+) \| Bytes=(.+)$') {
            $entries.Add([pscustomobject]@{
                Kind       = 'internal'
                Symbol     = $matches[1]
                Calls      = [int]$matches[2]
                SampleSite = [uint32]::Parse($matches[4].Substring(2), [System.Globalization.NumberStyles]::HexNumber)
                SampleText = $matches[4]
            }) | Out-Null
            continue
        }

        if ($line -match '^Import=([^|]+) \| Calls=(\d+) \| SampleSite=(0x[0-9A-F]+) \| Bytes=(.+)$') {
            $entries.Add([pscustomobject]@{
                Kind       = 'import'
                Symbol     = $matches[1].Trim()
                Calls      = [int]$matches[2]
                SampleSite = [uint32]::Parse($matches[3].Substring(2), [System.Globalization.NumberStyles]::HexNumber)
                SampleText = $matches[3]
            }) | Out-Null
        }
    }

    return @($entries | Sort-Object SampleSite)
}

function Get-WindowName {
    param([uint32]$SampleSite)

    if ($SampleSite -le 0x00036404) {
        return 'irql-and-shared-spine'
    }

    if ($SampleSite -le 0x000364CF) {
        return 'wrapper-and-time-anchor'
    }

    if ($SampleSite -le 0x000365D6) {
        return 'delayed-sleep-and-trace-side'
    }

    return 'terminal-side-bridges'
}

$repoRoot = Get-RepoRoot
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath
$deepPath = Join-Path $repoRoot 'out\dev\usbxhci-exhaustive-0003634C-deep.txt'

if (-not (Test-Path -LiteralPath $deepPath)) {
    throw "Required deep artifact not found: $deepPath"
}

$entries = Parse-WindowEntries -Path $deepPath
$windowNames = @(
    'irql-and-shared-spine',
    'wrapper-and-time-anchor',
    'delayed-sleep-and-trace-side',
    'terminal-side-bridges'
)

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI 3634C Window Map') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Function Envelope' -Body @(
    '- body: 0x0003634C-0x000366BC',
    '- source artifact: out\dev\usbxhci-exhaustive-0003634C-deep.txt',
    '- interpretation goal: group pacing-relevant and branch-heavy call sites into bounded windows'
)

foreach ($windowName in $windowNames) {
    $windowEntries = @($entries | Where-Object { (Get-WindowName -SampleSite $_.SampleSite) -eq $windowName })
    if ($windowEntries.Count -eq 0) {
        continue
    }

    $startSite = ('0x{0:X8}' -f $windowEntries[0].SampleSite)
    $endSite = ('0x{0:X8}' -f $windowEntries[-1].SampleSite)
    $body = New-Object 'System.Collections.Generic.List[string]'
    $body.Add("range: $startSite .. $endSite") | Out-Null

    foreach ($entry in $windowEntries) {
        if ($entry.Kind -eq 'import') {
            $body.Add("Import=$($entry.Symbol) | Calls=$($entry.Calls) | SampleSite=$($entry.SampleText)") | Out-Null
        }
        else {
            $body.Add("Target=$($entry.Symbol) | Calls=$($entry.Calls) | SampleSite=$($entry.SampleText)") | Out-Null
        }
    }

    switch ($windowName) {
        'irql-and-shared-spine' {
            $body.Add('role: initial IRQL sampling plus shared controller-spine branches before the time anchor') | Out-Null
        }
        'wrapper-and-time-anchor' {
            $body.Add('role: wrapper ladder, interrupt-time anchor, self-loop, and bridge setup before the delayed-sleep region') | Out-Null
        }
        'delayed-sleep-and-trace-side' {
            $body.Add('role: delayed thread sleep plus trace-side and debug-side controller context') | Out-Null
        }
        'terminal-side-bridges' {
            $body.Add('role: terminal bridge-to-stub and interrupt-side exits after the pacing region') | Out-Null
        }
    }

    Add-Section -Lines $lines -Title $windowName -Body $body
}

Add-Section -Lines $lines -Title 'Recommendation' -Body @(
    '- if any future host-side experiment uses 0x0003634C, treat these windows separately rather than as one body',
    '- highest-value pacing windows:',
    '  - irql-and-shared-spine',
    '  - wrapper-and-time-anchor',
    '  - delayed-sleep-and-trace-side',
    '- lowest-value window:',
    '  - terminal-side-bridges, because it is mostly bridge-to-stub and interrupt-side exit context'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
