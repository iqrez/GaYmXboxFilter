[CmdletBinding()]
param(
    [string]$PrimaryBodyPath,
    [string]$OpaqueSlabPath,
    [string]$PrimaryFollowPath,
    [string]$SecondaryBodyPath,
    [string]$SecondaryFollowPath,
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
    return Join-Path $outputDirectory 'usbxhci-next-five-assessment.txt'
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

$resolvedPrimaryBodyPath = Resolve-InputPath -RequestedPath $PrimaryBodyPath -DefaultName 'usbxhci-transfer-0D258-deep.txt'
$resolvedOpaqueSlabPath = Resolve-InputPath -RequestedPath $OpaqueSlabPath -DefaultName 'usbxhci-transfer-58BC0-deep.txt'
$resolvedPrimaryFollowPath = Resolve-InputPath -RequestedPath $PrimaryFollowPath -DefaultName 'usbxhci-transfer-0D59C-deep.txt'
$resolvedSecondaryBodyPath = Resolve-InputPath -RequestedPath $SecondaryBodyPath -DefaultName 'usbxhci-transfer-1BAC0-deep.txt'
$resolvedSecondaryFollowPath = Resolve-InputPath -RequestedPath $SecondaryFollowPath -DefaultName 'usbxhci-transfer-1BF58-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$primaryBodyProfile = Get-Profile -Path $resolvedPrimaryBodyPath
$opaqueSlabProfile = Get-Profile -Path $resolvedOpaqueSlabPath
$primaryFollowProfile = Get-Profile -Path $resolvedPrimaryFollowPath
$secondaryBodyProfile = Get-Profile -Path $resolvedSecondaryBodyPath
$secondaryFollowProfile = Get-Profile -Path $resolvedSecondaryFollowPath

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Next Five Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('PrimaryLine     : {0} -> {1}' -f $primaryBodyProfile.TargetHex, $primaryFollowProfile.TargetHex),
    ('SecondaryLine   : {0} -> {1}' -f $secondaryBodyProfile.TargetHex, $secondaryFollowProfile.TargetHex),
    ('OpaqueCandidate : {0}' -f $opaqueSlabProfile.TargetHex),
    ('RecommendedNext : {0}' -f $primaryFollowProfile.TargetHex),
    ('SecondaryTrack  : {0}' -f $secondaryBodyProfile.TargetHex)
)

Add-ProfileSection -Lines $lines -Title ("Primary Body {0}" -f $primaryBodyProfile.TargetHex) -Profile $primaryBodyProfile
Add-ProfileSection -Lines $lines -Title ("Opaque Candidate {0}" -f $opaqueSlabProfile.TargetHex) -Profile $opaqueSlabProfile
Add-ProfileSection -Lines $lines -Title ("Primary Follow-On {0}" -f $primaryFollowProfile.TargetHex) -Profile $primaryFollowProfile
Add-ProfileSection -Lines $lines -Title ("Secondary Body {0}" -f $secondaryBodyProfile.TargetHex) -Profile $secondaryBodyProfile
Add-ProfileSection -Lines $lines -Title ("Secondary Follow-On {0}" -f $secondaryFollowProfile.TargetHex) -Profile $secondaryFollowProfile

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It freezes the next five queued targets into a primary line, a secondary line, and one opaque side candidate.',
    'The goal is to pick the next deepest offline target while explicitly demoting the candidates that collapse into opaque slabs or trace-heavy wrappers.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI next five assessment written to {0}" -f $resolvedOutputPath)
