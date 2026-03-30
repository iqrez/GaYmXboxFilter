[CmdletBinding()]
param(
    [string]$ContinuationPath,
    [string]$BridgePath,
    [string]$ControlPath,
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
    return Join-Path $outputDirectory 'usbxhci-hot-body-branch-assessment.txt'
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

function Parse-NeighborFunctions {
    param(
        [string[]]$Content,
        [string]$TargetRvaHex
    )

    $header = "## Primary Neighbors $TargetRvaHex"
    $startIndex = [Array]::IndexOf($Content, $header)
    if ($startIndex -lt 0) {
        return @()
    }

    $items = @()
    for ($index = $startIndex + 1; $index -lt $Content.Length; $index++) {
        $line = $Content[$index]
        if ($line -like '## *') {
            break
        }

        if ($line -match '^Slot=([^|]+)\s+\|\s+Function=([^|]+)\s+\|') {
            $items += [pscustomobject]@{
                Slot     = $matches[1].Trim()
                Function = $matches[2].Trim()
            }
        }
    }

    return $items
}

function Get-BranchRole {
    param(
        [string]$TargetHex
    )

    switch ($TargetHex) {
        '0x00006E74' { return 'endpoint-side hot continuation' }
        '0x0000757C' { return 'quiet transfer bridge' }
        '0x00040D38' { return 'control/assert branch' }
        default { return 'unclassified branch' }
    }
}

$resolvedContinuationPath = Resolve-InputPath -RequestedPath $ContinuationPath -DefaultName 'usbxhci-single-target-deep.txt'
$resolvedBridgePath = Resolve-InputPath -RequestedPath $BridgePath -DefaultName 'usbxhci-helper-0757C-deep.txt'
$resolvedControlPath = Resolve-InputPath -RequestedPath $ControlPath -DefaultName 'usbxhci-helper-40D38-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$paths = @($resolvedContinuationPath, $resolvedBridgePath, $resolvedControlPath)
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
    $neighbors = Parse-NeighborFunctions -Content $content -TargetRvaHex $targetHex

    $profiles += [pscustomobject]@{
        TargetHex      = $targetHex
        Function       = $primary['Function']
        Size           = [int]$primary['Size']
        DirectInternal = [int]$primary['DirectInternal']
        DirectIat      = [int]$primary['DirectIat']
        InternalItems  = $internalItems
        ImportItems    = $importItems
        Neighbors      = $neighbors
        Role           = Get-BranchRole -TargetHex $targetHex
        Path           = $path
    }
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Hot Body Branch Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('InputCount      : {0}' -f $profiles.Count),
    ('BranchTargets   : {0}' -f ([string]::Join(', ', ($profiles | ForEach-Object { $_.TargetHex })))),
    'RecommendedNext : 0x00006E74'
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

    if ($profile.Neighbors.Count -gt 0) {
        $body += 'Neighbors:'
        $body += ($profile.Neighbors | ForEach-Object { "  Slot=$($_.Slot) | Function=$($_.Function)" })
    }

    if ($profile.InternalItems.Count -gt 0) {
        $body += 'InternalTargets:'
        $body += ($profile.InternalItems | ForEach-Object { "  $_" })
    }

    if ($profile.ImportItems.Count -gt 0) {
        $body += 'DirectImports:'
        $body += ($profile.ImportItems | ForEach-Object { "  $_" })
    }

    Add-Section -Lines $lines -Title ("Branch Candidate {0}" -f $profile.TargetHex) -Body $body
}

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It compares the three outward branches from the hot body at 0x00006BA0.',
    'The goal is to separate the substantive continuation, the quiet bridge into transfer-side code, and the likely control/assert side branch.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI hot body branch assessment written to {0}" -f $resolvedOutputPath)
