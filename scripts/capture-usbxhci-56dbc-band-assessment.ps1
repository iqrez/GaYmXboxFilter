[CmdletBinding()]
param(
    [string]$StubAPath,
    [string]$StubBPath,
    [string]$StubCPath,
    [string]$StubDPath,
    [string]$BridgePath,
    [string]$StringHelperPath,
    [string]$MixedHelperPath,
    [string]$PageHelperPath,
    [string]$AdjacentAPath,
    [string]$AdjacentBPath,
    [string]$AdjacentCPath,
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
    return Join-Path $outputDirectory 'usbxhci-56dbc-band-assessment.txt'
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

$resolvedStubAPath = Resolve-InputPath -RequestedPath $StubAPath -DefaultName 'usbxhci-transfer-01008-deep.txt'
$resolvedStubBPath = Resolve-InputPath -RequestedPath $StubBPath -DefaultName 'usbxhci-transfer-0103C-deep.txt'
$resolvedStubCPath = Resolve-InputPath -RequestedPath $StubCPath -DefaultName 'usbxhci-transfer-01068-deep.txt'
$resolvedStubDPath = Resolve-InputPath -RequestedPath $StubDPath -DefaultName 'usbxhci-transfer-58AC0-deep.txt'
$resolvedBridgePath = Resolve-InputPath -RequestedPath $BridgePath -DefaultName 'usbxhci-transfer-56B50-deep.txt'
$resolvedStringHelperPath = Resolve-InputPath -RequestedPath $StringHelperPath -DefaultName 'usbxhci-transfer-56BA0-deep.txt'
$resolvedMixedHelperPath = Resolve-InputPath -RequestedPath $MixedHelperPath -DefaultName 'usbxhci-transfer-56CA4-deep.txt'
$resolvedPageHelperPath = Resolve-InputPath -RequestedPath $PageHelperPath -DefaultName 'usbxhci-transfer-79C58-deep.txt'
$resolvedAdjacentAPath = Resolve-InputPath -RequestedPath $AdjacentAPath -DefaultName 'usbxhci-transfer-57610-deep.txt'
$resolvedAdjacentBPath = Resolve-InputPath -RequestedPath $AdjacentBPath -DefaultName 'usbxhci-transfer-57748-deep.txt'
$resolvedAdjacentCPath = Resolve-InputPath -RequestedPath $AdjacentCPath -DefaultName 'usbxhci-transfer-585E4-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$stubAProfile = Get-Profile -Path $resolvedStubAPath
$stubBProfile = Get-Profile -Path $resolvedStubBPath
$stubCProfile = Get-Profile -Path $resolvedStubCPath
$stubDProfile = Get-Profile -Path $resolvedStubDPath
$bridgeProfile = Get-Profile -Path $resolvedBridgePath
$stringHelperProfile = Get-Profile -Path $resolvedStringHelperPath
$mixedHelperProfile = Get-Profile -Path $resolvedMixedHelperPath
$pageHelperProfile = Get-Profile -Path $resolvedPageHelperPath
$adjacentAProfile = Get-Profile -Path $resolvedAdjacentAPath
$adjacentBProfile = Get-Profile -Path $resolvedAdjacentBPath
$adjacentCProfile = Get-Profile -Path $resolvedAdjacentCPath

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI 56DBC Band Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('StubBand        : {0}, {1}, {2}, {3}' -f $stubAProfile.TargetHex, $stubBProfile.TargetHex, $stubCProfile.TargetHex, $stubDProfile.TargetHex),
    ('HelperBand      : {0}, {1}, {2}, {3}' -f $bridgeProfile.TargetHex, $stringHelperProfile.TargetHex, $mixedHelperProfile.TargetHex, $pageHelperProfile.TargetHex),
    ('AdjacentBodies  : {0}, {1}, {2}' -f $adjacentAProfile.TargetHex, $adjacentBProfile.TargetHex, $adjacentCProfile.TargetHex),
    'RecommendedNext : 0x0001BAC0',
    'PrimaryBandState : registration/debug drift'
)

Add-ProfileSection -Lines $lines -Title ("Stub Candidate {0}" -f $stubAProfile.TargetHex) -Profile $stubAProfile
Add-ProfileSection -Lines $lines -Title ("Stub Candidate {0}" -f $stubBProfile.TargetHex) -Profile $stubBProfile
Add-ProfileSection -Lines $lines -Title ("Stub Candidate {0}" -f $stubCProfile.TargetHex) -Profile $stubCProfile
Add-ProfileSection -Lines $lines -Title ("Stub Candidate {0}" -f $stubDProfile.TargetHex) -Profile $stubDProfile
Add-ProfileSection -Lines $lines -Title ("Helper Candidate {0}" -f $bridgeProfile.TargetHex) -Profile $bridgeProfile
Add-ProfileSection -Lines $lines -Title ("Helper Candidate {0}" -f $stringHelperProfile.TargetHex) -Profile $stringHelperProfile
Add-ProfileSection -Lines $lines -Title ("Helper Candidate {0}" -f $mixedHelperProfile.TargetHex) -Profile $mixedHelperProfile
Add-ProfileSection -Lines $lines -Title ("Helper Candidate {0}" -f $pageHelperProfile.TargetHex) -Profile $pageHelperProfile
Add-ProfileSection -Lines $lines -Title ("Adjacent Body {0}" -f $adjacentAProfile.TargetHex) -Profile $adjacentAProfile
Add-ProfileSection -Lines $lines -Title ("Adjacent Body {0}" -f $adjacentBProfile.TargetHex) -Profile $adjacentBProfile
Add-ProfileSection -Lines $lines -Title ("Adjacent Body {0}" -f $adjacentCProfile.TargetHex) -Profile $adjacentCProfile

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It assesses whether the 0x00056DBC band still points at a promising hot-path body or has drifted into registration, path, ETW, and cleanup machinery.',
    'The goal is to decide whether to keep following this primary band or pivot back to the cleaner secondary branch.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI 56DBC band assessment written to {0}" -f $resolvedOutputPath)
