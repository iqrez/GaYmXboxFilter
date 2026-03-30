[CmdletBinding()]
param(
    [string]$TraceFollowPath,
    [string]$BridgePath,
    [string]$InstrumentedPath,
    [string]$BridgeFollowPath,
    [string]$ResolvedBodyPath,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-InputPath {
    param(
        [string]$RequestedPath,
        [string]$DefaultName
    )

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Join-Path (Get-RepoRoot) 'out\dev') $DefaultName
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    $outputDirectory = Join-Path (Get-RepoRoot) 'out\dev'
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    return Join-Path $outputDirectory 'usbxhci-d59c-follow-assessment.txt'
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

function Parse-ScalarValue {
    param(
        [string[]]$Content,
        [string]$Key
    )

    foreach ($line in $Content) {
        if ($line -match ('^{0}\s+:\s+(.+)$' -f [Regex]::Escape($Key))) {
            return $matches[1].Trim()
        }
    }

    return $null
}

function Parse-PrimaryBlock {
    param(
        [string[]]$Content,
        [string]$TargetHex
    )

    $header = "## Primary Target $TargetHex"
    $startIndex = [Array]::IndexOf($Content, $header)
    if ($startIndex -lt 0) {
        throw "Primary block not found for $TargetHex."
    }

    $result = [ordered]@{}
    for ($index = $startIndex + 1; $index -lt $Content.Length; $index++) {
        $line = $Content[$index]
        if ($line -like '## *') {
            break
        }

        if ($line -match '^\s*([^:]+?)\s*:\s*(.+)$') {
            $result[$matches[1].Trim()] = $matches[2].Trim()
        }
    }

    return $result
}

function Parse-SectionItems {
    param(
        [string[]]$Content,
        [string]$Header
    )

    $startIndex = [Array]::IndexOf($Content, $Header)
    if ($startIndex -lt 0) {
        return @()
    }

    $items = @()
    for ($index = $startIndex + 1; $index -lt $Content.Length; $index++) {
        $line = $Content[$index]
        if ($line -like '## *') {
            break
        }

        if ($line -match '^Target=0x[0-9A-F]+' -or $line -match '^Import=') {
            $items += $line.Trim()
        }
    }

    return $items
}

function Get-Profile {
    param([string]$Path)

    $content = Get-Content -Path $Path
    $targetHex = Parse-ScalarValue -Content $content -Key 'TargetRva'
    if (-not $targetHex) {
        throw "TargetRva not found in $Path."
    }

    $primary = Parse-PrimaryBlock -Content $content -TargetHex $targetHex
    return [pscustomobject]@{
        Path           = $Path
        TargetHex      = $targetHex
        Function       = $primary['Function']
        Size           = [int]$primary['Size']
        DirectInternal = [int]$primary['DirectInternal']
        DirectIat      = [int]$primary['DirectIat']
        InternalItems  = Parse-SectionItems -Content $content -Header ("## Primary Internal {0}" -f $targetHex)
        ImportItems    = Parse-SectionItems -Content $content -Header ("## Primary IAT {0}" -f $targetHex)
    }
}

function Add-ProfileSection {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Title,
        [object]$Profile
    )

    $body = @(
        ('Path          : {0}' -f $Profile.Path),
        ('Function      : {0}' -f $Profile.Function),
        ('Size          : {0}' -f $Profile.Size),
        ('DirectInternal: {0}' -f $Profile.DirectInternal),
        ('DirectIat     : {0}' -f $Profile.DirectIat)
    )

    if ($Profile.InternalItems.Count -gt 0) {
        $body += 'InternalTargets:'
        $body += ($Profile.InternalItems | ForEach-Object { "  $_" })
    }

    if ($Profile.ImportItems.Count -gt 0) {
        $body += 'DirectImports:'
        $body += ($Profile.ImportItems | ForEach-Object { "  $_" })
    }

    Add-Section -Lines $Lines -Title $Title -Body $body
}

$resolvedTraceFollowPath = Resolve-InputPath -RequestedPath $TraceFollowPath -DefaultName 'usbxhci-transfer-1BF58-deep.txt'
$resolvedBridgePath = Resolve-InputPath -RequestedPath $BridgePath -DefaultName 'usbxhci-transfer-0D210-deep.txt'
$resolvedInstrumentedPath = Resolve-InputPath -RequestedPath $InstrumentedPath -DefaultName 'usbxhci-transfer-1A7FC-deep.txt'
$resolvedBridgeFollowPath = Resolve-InputPath -RequestedPath $BridgeFollowPath -DefaultName 'usbxhci-transfer-56D8C-deep.txt'
$resolvedResolvedBodyPath = Resolve-InputPath -RequestedPath $ResolvedBodyPath -DefaultName 'usbxhci-transfer-56DBC-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$traceFollowProfile = Get-Profile -Path $resolvedTraceFollowPath
$bridgeProfile = Get-Profile -Path $resolvedBridgePath
$instrumentedProfile = Get-Profile -Path $resolvedInstrumentedPath
$bridgeFollowProfile = Get-Profile -Path $resolvedBridgeFollowPath
$resolvedBodyProfile = Get-Profile -Path $resolvedResolvedBodyPath

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI D59C Follow Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('ComparedTargets : {0}, {1}, {2}, {3}, {4}' -f $traceFollowProfile.TargetHex, $bridgeProfile.TargetHex, $instrumentedProfile.TargetHex, $bridgeFollowProfile.TargetHex, $resolvedBodyProfile.TargetHex),
    ('RecommendedNext : {0}' -f $resolvedBodyProfile.TargetHex)
)

Add-ProfileSection -Lines $lines -Title ("Trace Follow-On {0}" -f $traceFollowProfile.TargetHex) -Profile $traceFollowProfile
Add-ProfileSection -Lines $lines -Title ("Bridge Follow-On {0}" -f $bridgeProfile.TargetHex) -Profile $bridgeProfile
Add-ProfileSection -Lines $lines -Title ("Instrumented Follow-On {0}" -f $instrumentedProfile.TargetHex) -Profile $instrumentedProfile
Add-ProfileSection -Lines $lines -Title ("Bridge Leg {0}" -f $bridgeFollowProfile.TargetHex) -Profile $bridgeFollowProfile
Add-ProfileSection -Lines $lines -Title ("Resolved Body {0}" -f $resolvedBodyProfile.TargetHex) -Profile $resolvedBodyProfile

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It resolves the current 0x0000D59C follow-on set by demoting the trace-heavy, instrumented, and bridge-only legs.',
    'The goal is to identify the first substantive body that remains after those bridge and wrapper paths are stripped away.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI D59C follow assessment written to {0}" -f $resolvedOutputPath)
