[CmdletBinding()]
param(
    [string]$Helper1Path,
    [string]$Helper2Path,
    [string]$Helper3Path,
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
    return Join-Path $outputDirectory 'usbxhci-shared-helper-assessment.txt'
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

        if ($line -match '^Target=0x[0-9A-F]+') {
            $items += $line.Trim()
        }
        elseif ($line -match '^Import=') {
            $items += $line.Trim()
        }
    }

    return $items
}

function Get-Assessment {
    param(
        [string]$TargetHex,
        [string[]]$InternalItems,
        [string[]]$ImportItems
    )

    if ($TargetHex -eq '0x00006A44') {
        return 'hot helper continuation'
    }

    if ($ImportItems | Where-Object { $_ -like 'Import=WppRecorder.sys!WppAutoLogTrace*' }) {
        return 'instrumented wrapper'
    }

    if ($ImportItems | Where-Object { $_ -like 'Import=ntoskrnl.exe!DbgPrint*' -or $_ -like 'Import=ntoskrnl.exe!KdRefreshDebuggerNotPresent*' }) {
        return 'debug-heavy seam'
    }

    return 'unclassified helper'
}

$resolvedHelper1Path = Resolve-InputPath -RequestedPath $Helper1Path -DefaultName 'usbxhci-helper-1F9A4-deep.txt'
$resolvedHelper2Path = Resolve-InputPath -RequestedPath $Helper2Path -DefaultName 'usbxhci-helper-06A44-deep.txt'
$resolvedHelper3Path = Resolve-InputPath -RequestedPath $Helper3Path -DefaultName 'usbxhci-helper-049B4-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$targets = @(
    [pscustomobject]@{
        Path = $resolvedHelper1Path
    },
    [pscustomobject]@{
        Path = $resolvedHelper2Path
    },
    [pscustomobject]@{
        Path = $resolvedHelper3Path
    }
)

$profiles = @()
foreach ($target in $targets) {
    $content = Get-Content -Path $target.Path
    $targetHex = Parse-ScalarValue -Content $content -Key 'TargetRva'
    if (-not $targetHex) {
        throw "TargetRva not found in $($target.Path)."
    }

    $primary = Parse-PrimaryBlock -Content $content -TargetRvaHex $targetHex
    $internalItems = Parse-SectionItems -Content $content -Header ("## Primary Internal {0}" -f $targetHex)
    $importItems = Parse-SectionItems -Content $content -Header ("## Primary IAT {0}" -f $targetHex)

    $profiles += [pscustomobject]@{
        TargetHex    = $targetHex
        Function     = $primary['Function']
        Size         = $primary['Size']
        DirectInternal = $primary['DirectInternal']
        DirectIat    = $primary['DirectIat']
        InternalItems = $internalItems
        ImportItems  = $importItems
        Assessment   = Get-Assessment -TargetHex $targetHex -InternalItems $internalItems -ImportItems $importItems
        Path         = $target.Path
    }
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Shared Helper Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('InputCount      : {0}' -f $profiles.Count),
    ('PrimaryTargets  : {0}' -f ([string]::Join(', ', ($profiles | ForEach-Object { $_.TargetHex })))),
    ('RecommendedNext : 0x00006A44')
)

foreach ($profile in $profiles) {
    $body = @(
        ('Path          : {0}' -f $profile.Path),
        ('Function      : {0}' -f $profile.Function),
        ('Size          : {0}' -f $profile.Size),
        ('DirectInternal: {0}' -f $profile.DirectInternal),
        ('DirectIat     : {0}' -f $profile.DirectIat),
        ('Assessment    : {0}' -f $profile.Assessment)
    )

    if ($profile.InternalItems.Count -gt 0) {
        $body += 'InternalTargets:'
        $body += ($profile.InternalItems | ForEach-Object { "  $_" })
    }

    if ($profile.ImportItems.Count -gt 0) {
        $body += 'DirectImports:'
        $body += ($profile.ImportItems | ForEach-Object { "  $_" })
    }

    Add-Section -Lines $lines -Title ("Candidate {0}" -f $profile.TargetHex) -Body $body
}

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It compares the three shared-helper candidates selected from the endpoint-neighbor comparison.',
    'The goal is to separate the remaining hot helper continuation from wrapper or debug-heavy seam code before any deeper offline pass.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI shared helper assessment written to {0}" -f $resolvedOutputPath)
