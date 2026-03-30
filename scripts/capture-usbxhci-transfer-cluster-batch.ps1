[CmdletBinding()]
param(
    [uint32[]]$TargetRvas = @(
        [uint32]0x000076A0,
        [uint32]0x000077FC,
        [uint32]0x00007B70,
        [uint32]0x00007D60,
        [uint32]0x00008878
    ),
    [string]$DeepScriptPath,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-DeepScriptPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path $PSScriptRoot 'capture-usbxhci-single-target-deep.ps1'
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    $outputDirectory = Join-Path (Get-RepoRoot) 'out\dev'
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    return Join-Path $outputDirectory 'usbxhci-transfer-cluster-batch.txt'
}

function Get-DeepOutputPath {
    param([uint32]$TargetRva)

    $outputDirectory = Join-Path (Get-RepoRoot) 'out\dev'
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    return (Join-Path $outputDirectory ('usbxhci-transfer-{0:X5}-deep.txt' -f $TargetRva))
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

function Parse-NeighborFunctions {
    param(
        [string[]]$Content,
        [string]$TargetHex
    )

    $header = "## Primary Neighbors $TargetHex"
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

function Get-TargetHex {
    param([uint32]$TargetRva)
    return ('0x{0:X8}' -f $TargetRva)
}

$resolvedDeepScriptPath = Resolve-DeepScriptPath -RequestedPath $DeepScriptPath
if (-not (Test-Path -LiteralPath $resolvedDeepScriptPath)) {
    throw "Deep capture script not found: $resolvedDeepScriptPath"
}

$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath
$profiles = @()

foreach ($targetRva in $TargetRvas) {
    $deepOutputPath = Get-DeepOutputPath -TargetRva $targetRva
    & $resolvedDeepScriptPath -TargetRva $targetRva -FollowTargetRvas @() -OutputPath $deepOutputPath

    $content = Get-Content -Path $deepOutputPath
    $targetHex = Parse-ScalarValue -Content $content -Key 'TargetRva'
    $primary = Parse-PrimaryBlock -Content $content -TargetHex $targetHex
    $internalItems = Parse-SectionItems -Content $content -Header ("## Primary Internal {0}" -f $targetHex)
    $importItems = Parse-SectionItems -Content $content -Header ("## Primary IAT {0}" -f $targetHex)
    $neighbors = Parse-NeighborFunctions -Content $content -TargetHex $targetHex

    $score = ([int]$primary['DirectInternal'] * 100) + ([int]$primary['DirectIat'] * 25) + [int]$primary['Size']

    $profiles += [pscustomobject]@{
        TargetHex      = $targetHex
        Function       = $primary['Function']
        Size           = [int]$primary['Size']
        DirectInternal = [int]$primary['DirectInternal']
        DirectIat      = [int]$primary['DirectIat']
        InternalItems  = $internalItems
        ImportItems    = $importItems
        Neighbors      = $neighbors
        Score          = $score
        Path           = $deepOutputPath
    }
}

$rankedProfiles = $profiles | Sort-Object @{ Expression = 'Score'; Descending = $true }, @{ Expression = 'DirectInternal'; Descending = $true }, @{ Expression = 'DirectIat'; Descending = $true }, @{ Expression = 'Size'; Descending = $true }
$recommended = $rankedProfiles | Select-Object -First 1

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Transfer Cluster Batch') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('TargetCount      : {0}' -f $profiles.Count),
    ('Targets          : {0}' -f ([string]::Join(', ', ($profiles | ForEach-Object { $_.TargetHex })))),
    ('RecommendedNext  : {0}' -f $recommended.TargetHex),
    ('RecommendationBy : score={0}, direct_internal={1}, direct_iat={2}, size={3}' -f $recommended.Score, $recommended.DirectInternal, $recommended.DirectIat, $recommended.Size)
)

$rankBody = @()
foreach ($profile in $rankedProfiles) {
    $rankBody += ('Target={0} | Score={1} | DirectInternal={2} | DirectIat={3} | Size={4}' -f $profile.TargetHex, $profile.Score, $profile.DirectInternal, $profile.DirectIat, $profile.Size)
}
Add-Section -Lines $lines -Title 'Ranking' -Body $rankBody

foreach ($profile in $rankedProfiles) {
    $body = @(
        ('Path          : {0}' -f $profile.Path),
        ('Function      : {0}' -f $profile.Function),
        ('Size          : {0}' -f $profile.Size),
        ('DirectInternal: {0}' -f $profile.DirectInternal),
        ('DirectIat     : {0}' -f $profile.DirectIat),
        ('Score         : {0}' -f $profile.Score)
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

    Add-Section -Lines $lines -Title ("Candidate {0}" -f $profile.TargetHex) -Body $body
}

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It batch-captures the transfer-side neighborhood around 0x000077FC and ranks the resulting bodies.',
    'The goal is to support a larger offline test without moving back into live host-stack perturbation.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI transfer cluster batch written to {0}" -f $resolvedOutputPath)
