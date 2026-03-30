[CmdletBinding()]
param(
    [string]$ParentPath,
    [string]$StubPath,
    [string]$BodyPath,
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
    return Join-Path $outputDirectory 'usbxhci-transfer-next-hop-assessment.txt'
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

$resolvedParentPath = Resolve-InputPath -RequestedPath $ParentPath -DefaultName 'usbxhci-transfer-08180-deep.txt'
$resolvedStubPath = Resolve-InputPath -RequestedPath $StubPath -DefaultName 'usbxhci-transfer-08E74-deep.txt'
$resolvedBodyPath = Resolve-InputPath -RequestedPath $BodyPath -DefaultName 'usbxhci-transfer-46530-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$parentProfile = Get-Profile -Path $resolvedParentPath
$stubProfile = Get-Profile -Path $resolvedStubPath
$bodyProfile = Get-Profile -Path $resolvedBodyPath

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Transfer Next-Hop Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('ParentTarget    : {0}' -f $parentProfile.TargetHex),
    ('NextHopTargets  : {0}, {1}' -f $stubProfile.TargetHex, $bodyProfile.TargetHex),
    ('RecommendedNext : {0}' -f $bodyProfile.TargetHex)
)

Add-Section -Lines $lines -Title ("Parent {0}" -f $parentProfile.TargetHex) -Body @(
    ('Function      : {0}' -f $parentProfile.Function),
    ('Size          : {0}' -f $parentProfile.Size),
    ('DirectInternal: {0}' -f $parentProfile.DirectInternal),
    ('DirectIat     : {0}' -f $parentProfile.DirectIat)
)

foreach ($profile in @($stubProfile, $bodyProfile)) {
    $body = @(
        ('Path          : {0}' -f $profile.Path),
        ('Function      : {0}' -f $profile.Function),
        ('Size          : {0}' -f $profile.Size),
        ('DirectInternal: {0}' -f $profile.DirectInternal),
        ('DirectIat     : {0}' -f $profile.DirectIat)
    )

    if ($profile.InternalItems.Count -gt 0) {
        $body += 'InternalTargets:'
        $body += ($profile.InternalItems | ForEach-Object { "  $_" })
    }

    if ($profile.ImportItems.Count -gt 0) {
        $body += 'DirectImports:'
        $body += ($profile.ImportItems | ForEach-Object { "  $_" })
    }

    Add-Section -Lines $lines -Title ("Next-Hop Candidate {0}" -f $profile.TargetHex) -Body $body
}

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It compares the two direct next-hop bodies from 0x00008180.',
    'The goal is to separate a tiny local stub from the richer continuation body before any deeper offline pass.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI transfer next-hop assessment written to {0}" -f $resolvedOutputPath)
