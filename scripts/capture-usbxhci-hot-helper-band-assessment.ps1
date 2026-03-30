[CmdletBinding()]
param(
    [string]$EntryPath,
    [string]$PrimaryPath,
    [string]$TailPath,
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
    return Join-Path $outputDirectory 'usbxhci-hot-helper-band-assessment.txt'
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
        [string]$TargetRvaHex
    )

    $header = "## Primary Target $TargetRvaHex"
    $startIndex = [Array]::IndexOf($Content, $header)
    if ($startIndex -lt 0) {
        throw "Primary block not found for $TargetRvaHex."
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

function Get-BandRole {
    param(
        [string]$TargetHex,
        [string[]]$InternalItems,
        [string[]]$ImportItems
    )

    switch ($TargetHex) {
        '0x00006A44' { return 'entry helper' }
        '0x00006BA0' { return 'primary hot body' }
        '0x00007160' { return 'thin tail/thunk path' }
        default { return 'unclassified' }
    }
}

$resolvedEntryPath = Resolve-InputPath -RequestedPath $EntryPath -DefaultName 'usbxhci-helper-06A44-deep.txt'
$resolvedPrimaryPath = Resolve-InputPath -RequestedPath $PrimaryPath -DefaultName 'usbxhci-helper-06BA0-deep.txt'
$resolvedTailPath = Resolve-InputPath -RequestedPath $TailPath -DefaultName 'usbxhci-helper-07160-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$paths = @($resolvedEntryPath, $resolvedPrimaryPath, $resolvedTailPath)
$profiles = @()

foreach ($path in $paths) {
    $content = Get-Content -Path $path
    $targetHex = Parse-ScalarValue -Content $content -Key 'TargetRva'
    if (-not $targetHex) {
        throw "TargetRva not found in $path."
    }

    $primary = Parse-PrimaryBlock -Content $content -TargetRvaHex $targetHex
    $internalItems = Parse-SectionItems -Content $content -Header ("## Primary Internal {0}" -f $targetHex)
    $importItems = Parse-SectionItems -Content $content -Header ("## Primary IAT {0}" -f $targetHex)

    $profiles += [pscustomobject]@{
        TargetHex      = $targetHex
        Function       = $primary['Function']
        Size           = [int]$primary['Size']
        DirectInternal = [int]$primary['DirectInternal']
        DirectIat      = [int]$primary['DirectIat']
        InternalItems  = $internalItems
        ImportItems    = $importItems
        Role           = Get-BandRole -TargetHex $targetHex -InternalItems $internalItems -ImportItems $importItems
        Path           = $path
    }
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Hot Helper Band Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('InputCount      : {0}' -f $profiles.Count),
    ('BandTargets     : {0}' -f ([string]::Join(', ', ($profiles | ForEach-Object { $_.TargetHex })))),
    'RecommendedNext : 0x00006BA0'
)

foreach ($profile in $profiles) {
    $body = @(
        ('Path          : {0}' -f $profile.Path),
        ('Function      : {0}' -f $profile.Function),
        ('Size          : {0}' -f $profile.Size),
        ('DirectInternal: {0}' -f $profile.DirectInternal),
        ('DirectIat     : {0}' -f $profile.DirectIat),
        ('Role          : {0}' -f $profile.Role)
    )

    if ($profile.InternalItems.Count -gt 0) {
        $body += 'InternalTargets:'
        $body += ($profile.InternalItems | ForEach-Object { "  $_" })
    }

    if ($profile.ImportItems.Count -gt 0) {
        $body += 'DirectImports:'
        $body += ($profile.ImportItems | ForEach-Object { "  $_" })
    }

    Add-Section -Lines $lines -Title ("Band Candidate {0}" -f $profile.TargetHex) -Body $body
}

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It compares the three local members of the surviving hot-helper band around 0x00006A44.',
    'The goal is to separate the entry helper, the primary hot body, and the thin tail/thunk path before any deeper offline pass.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI hot helper band assessment written to {0}" -f $resolvedOutputPath)
