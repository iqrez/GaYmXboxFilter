[CmdletBinding()]
param(
    [string]$BridgePath,
    [string]$WrapperAPath,
    [string]$WrapperBPath,
    [string]$WrapperCPath,
    [string]$BodyAPath,
    [string]$BodyBPath,
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
    return Join-Path $outputDirectory 'usbxhci-timer-body-follow-assessment.txt'
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

$resolvedBridgePath = Resolve-InputPath -RequestedPath $BridgePath -DefaultName 'usbxhci-transfer-0D210-deep.txt'
$resolvedWrapperAPath = Resolve-InputPath -RequestedPath $WrapperAPath -DefaultName 'usbxhci-transfer-1BA28-deep.txt'
$resolvedWrapperBPath = Resolve-InputPath -RequestedPath $WrapperBPath -DefaultName 'usbxhci-transfer-1BA64-deep.txt'
$resolvedWrapperCPath = Resolve-InputPath -RequestedPath $WrapperCPath -DefaultName 'usbxhci-transfer-1BA8C-deep.txt'
$resolvedBodyAPath = Resolve-InputPath -RequestedPath $BodyAPath -DefaultName 'usbxhci-transfer-0D258-deep.txt'
$resolvedBodyBPath = Resolve-InputPath -RequestedPath $BodyBPath -DefaultName 'usbxhci-transfer-1BAC0-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$bridgeProfile = Get-Profile -Path $resolvedBridgePath
$wrapperAProfile = Get-Profile -Path $resolvedWrapperAPath
$wrapperBProfile = Get-Profile -Path $resolvedWrapperBPath
$wrapperCProfile = Get-Profile -Path $resolvedWrapperCPath
$bodyAProfile = Get-Profile -Path $resolvedBodyAPath
$bodyBProfile = Get-Profile -Path $resolvedBodyBPath

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Timer Body Follow Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('BridgeCandidate : {0}' -f $bridgeProfile.TargetHex),
    ('WrapperBand     : {0}, {1}, {2}' -f $wrapperAProfile.TargetHex, $wrapperBProfile.TargetHex, $wrapperCProfile.TargetHex),
    ('BodyCandidates  : {0}, {1}' -f $bodyAProfile.TargetHex, $bodyBProfile.TargetHex),
    ('RecommendedNext : {0}' -f $bodyAProfile.TargetHex),
    ('SecondaryTrack  : {0}' -f $bodyBProfile.TargetHex)
)

Add-ProfileSection -Lines $lines -Title ("Bridge Candidate {0}" -f $bridgeProfile.TargetHex) -Profile $bridgeProfile
Add-ProfileSection -Lines $lines -Title ("Wrapper Candidate {0}" -f $wrapperAProfile.TargetHex) -Profile $wrapperAProfile
Add-ProfileSection -Lines $lines -Title ("Wrapper Candidate {0}" -f $wrapperBProfile.TargetHex) -Profile $wrapperBProfile
Add-ProfileSection -Lines $lines -Title ("Wrapper Candidate {0}" -f $wrapperCProfile.TargetHex) -Profile $wrapperCProfile
Add-ProfileSection -Lines $lines -Title ("Body Candidate {0}" -f $bodyAProfile.TargetHex) -Profile $bodyAProfile
Add-ProfileSection -Lines $lines -Title ("Body Candidate {0}" -f $bodyBProfile.TargetHex) -Profile $bodyBProfile

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It compares the direct timer/orchestration bridge out of 0x0001B1F0, the tiny local wrapper ladder on the alternate side, and the first substantive bodies each side reaches.',
    'The goal is to decide whether the next offline focus should stay on the direct bridge continuation or switch to the local secondary branch.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI timer body follow assessment written to {0}" -f $resolvedOutputPath)
