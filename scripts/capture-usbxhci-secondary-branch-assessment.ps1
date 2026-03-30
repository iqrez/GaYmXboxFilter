[CmdletBinding()]
param(
    [string]$PrimaryBodyPath,
    [string]$TraceAPath,
    [string]$TraceBPath,
    [string]$BridgeAPath,
    [string]$BridgeBPath,
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
    return Join-Path $outputDirectory 'usbxhci-secondary-branch-assessment.txt'
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

$resolvedPrimaryBodyPath = Resolve-InputPath -RequestedPath $PrimaryBodyPath -DefaultName 'usbxhci-transfer-1BC34-deep.txt'
$resolvedTraceAPath = Resolve-InputPath -RequestedPath $TraceAPath -DefaultName 'usbxhci-transfer-1BF58-deep.txt'
$resolvedTraceBPath = Resolve-InputPath -RequestedPath $TraceBPath -DefaultName 'usbxhci-transfer-1F9A4-deep.txt'
$resolvedBridgeAPath = Resolve-InputPath -RequestedPath $BridgeAPath -DefaultName 'usbxhci-transfer-05BC0-deep.txt'
$resolvedBridgeBPath = Resolve-InputPath -RequestedPath $BridgeBPath -DefaultName 'usbxhci-transfer-06A08-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$primaryBodyProfile = Get-Profile -Path $resolvedPrimaryBodyPath
$traceAProfile = Get-Profile -Path $resolvedTraceAPath
$traceBProfile = Get-Profile -Path $resolvedTraceBPath
$bridgeAProfile = Get-Profile -Path $resolvedBridgeAPath
$bridgeBProfile = Get-Profile -Path $resolvedBridgeBPath

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Secondary Branch Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('ComparedTargets : {0}, {1}, {2}, {3}, {4}' -f $primaryBodyProfile.TargetHex, $traceAProfile.TargetHex, $traceBProfile.TargetHex, $bridgeAProfile.TargetHex, $bridgeBProfile.TargetHex),
    ('RecommendedNext : {0}' -f $primaryBodyProfile.TargetHex)
)

Add-ProfileSection -Lines $lines -Title ("Primary Body {0}" -f $primaryBodyProfile.TargetHex) -Profile $primaryBodyProfile
Add-ProfileSection -Lines $lines -Title ("Trace Leg {0}" -f $traceAProfile.TargetHex) -Profile $traceAProfile
Add-ProfileSection -Lines $lines -Title ("Trace Leg {0}" -f $traceBProfile.TargetHex) -Profile $traceBProfile
Add-ProfileSection -Lines $lines -Title ("Bridge Leg {0}" -f $bridgeAProfile.TargetHex) -Profile $bridgeAProfile
Add-ProfileSection -Lines $lines -Title ("Bridge Leg {0}" -f $bridgeBProfile.TargetHex) -Profile $bridgeBProfile

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It freezes the current 0x0001BAC0 secondary branch split into one substantive IRP/completion body, two trace-heavy side legs, and two small reconnectors into older helper machinery.',
    'The goal is to confirm whether the branch pivot away from the 0x00056DBC band should stay on 0x0001BC34.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI secondary branch assessment written to {0}" -f $resolvedOutputPath)
