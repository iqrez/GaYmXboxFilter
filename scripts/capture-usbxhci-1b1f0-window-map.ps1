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

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-1b1f0-window-map.txt'
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
                Detail     = $matches[3].Trim()
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
                Detail     = ''
            }) | Out-Null
        }
    }

    return @($entries | Sort-Object SampleSite)
}

function Get-WindowName {
    param([uint32]$SampleSite)

    if ($SampleSite -le 0x0001B30A) {
        return 'setup-and-bridge'
    }

    if ($SampleSite -le 0x0001B405) {
        return 'time-sampling-and-debug-gate'
    }

    if ($SampleSite -le 0x0001B599) {
        return 'timer-lifecycle-and-side-context'
    }

    return 'final-handoff'
}

$repoRoot = Get-RepoRoot
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath
$deepPath = Join-Path $repoRoot 'out\dev\usbxhci-exhaustive-0001B1F0-deep.txt'

if (-not (Test-Path -LiteralPath $deepPath)) {
    throw "Required deep artifact not found: $deepPath"
}

$entries = Parse-WindowEntries -Path $deepPath
$windowNames = @(
    'setup-and-bridge',
    'time-sampling-and-debug-gate',
    'timer-lifecycle-and-side-context',
    'final-handoff'
)

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# USBXHCI 1B1F0 Window Map') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Function Envelope' -Body @(
    '- body: 0x0001B1F0-0x0001B6D9',
    '- source artifact: out\dev\usbxhci-exhaustive-0001B1F0-deep.txt',
    '- interpretation goal: group timing-relevant and branch-heavy call sites into bounded windows'
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
        'setup-and-bridge' {
            $body.Add('role: wrapper ladder, self-loop, and alternate bridge setup before the time-sampling region') | Out-Null
        }
        'time-sampling-and-debug-gate' {
            $body.Add('role: interrupt-time reads, explicit stall timing, and nearby debug-side gates') | Out-Null
        }
        'timer-lifecycle-and-side-context' {
            $body.Add('role: timer allocation/setup/wait flow plus trace/debug side-context branching') | Out-Null
        }
        'final-handoff' {
            $body.Add('role: terminal trace-side branch, direct timing-descendant handoff to 0x0003FC38, and timer teardown') | Out-Null
        }
    }

    Add-Section -Lines $lines -Title $windowName -Body $body
}

Add-Section -Lines $lines -Title 'Recommendation' -Body @(
    '- if any future host-side experiment is scoped around 0x0001B1F0, treat these windows separately rather than treating the whole body as one unit',
    '- highest-value timing windows:',
    '  - time-sampling-and-debug-gate',
    '  - timer-lifecycle-and-side-context',
    '  - final-handoff',
    '- lowest-value timing window:',
    '  - setup-and-bridge, because it is dominated by wrapper and bridge setup rather than timing primitives'
)

Set-Content -Path $outputPath -Value $lines -Encoding ASCII
Write-Output "Wrote $outputPath"
