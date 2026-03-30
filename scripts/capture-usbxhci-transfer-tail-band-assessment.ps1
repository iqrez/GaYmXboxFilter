[CmdletBinding()]
param(
    [string]$TailAPath,
    [string]$TailBPath,
    [string]$TraceAPath,
    [string]$TraceBPath,
    [string]$SharedStubPath,
    [string]$SharedTracePath,
    [string]$NeighborBodyPath,
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
    return Join-Path $outputDirectory 'usbxhci-transfer-tail-band-assessment.txt'
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

$resolvedTailAPath = Resolve-InputPath -RequestedPath $TailAPath -DefaultName 'usbxhci-transfer-1B0D8-deep.txt'
$resolvedTailBPath = Resolve-InputPath -RequestedPath $TailBPath -DefaultName 'usbxhci-transfer-1B158-deep.txt'
$resolvedTraceAPath = Resolve-InputPath -RequestedPath $TraceAPath -DefaultName 'usbxhci-transfer-55790-deep.txt'
$resolvedTraceBPath = Resolve-InputPath -RequestedPath $TraceBPath -DefaultName 'usbxhci-transfer-55864-deep.txt'
$resolvedSharedStubPath = Resolve-InputPath -RequestedPath $SharedStubPath -DefaultName 'usbxhci-transfer-58AC0-deep.txt'
$resolvedSharedTracePath = Resolve-InputPath -RequestedPath $SharedTracePath -DefaultName 'usbxhci-transfer-0C8C0-deep.txt'
$resolvedNeighborBodyPath = Resolve-InputPath -RequestedPath $NeighborBodyPath -DefaultName 'usbxhci-transfer-1B1F0-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$tailAProfile = Get-Profile -Path $resolvedTailAPath
$tailBProfile = Get-Profile -Path $resolvedTailBPath
$traceAProfile = Get-Profile -Path $resolvedTraceAPath
$traceBProfile = Get-Profile -Path $resolvedTraceBPath
$sharedStubProfile = Get-Profile -Path $resolvedSharedStubPath
$sharedTraceProfile = Get-Profile -Path $resolvedSharedTracePath
$neighborBodyProfile = Get-Profile -Path $resolvedNeighborBodyPath

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Transfer Tail Band Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('TailPair       : {0}, {1}' -f $tailAProfile.TargetHex, $tailBProfile.TargetHex),
    ('TracePair      : {0}, {1}' -f $traceAProfile.TargetHex, $traceBProfile.TargetHex),
    ('SharedTargets  : {0}, {1}' -f $sharedStubProfile.TargetHex, $sharedTraceProfile.TargetHex),
    ('RecommendedNext: {0}' -f $neighborBodyProfile.TargetHex)
)

Add-ProfileSection -Lines $lines -Title ("Tail Candidate {0}" -f $tailAProfile.TargetHex) -Profile $tailAProfile
Add-ProfileSection -Lines $lines -Title ("Tail Candidate {0}" -f $tailBProfile.TargetHex) -Profile $tailBProfile
Add-ProfileSection -Lines $lines -Title ("Trace Candidate {0}" -f $traceAProfile.TargetHex) -Profile $traceAProfile
Add-ProfileSection -Lines $lines -Title ("Trace Candidate {0}" -f $traceBProfile.TargetHex) -Profile $traceBProfile
Add-ProfileSection -Lines $lines -Title ("Shared Target {0}" -f $sharedStubProfile.TargetHex) -Profile $sharedStubProfile
Add-ProfileSection -Lines $lines -Title ("Shared Target {0}" -f $sharedTraceProfile.TargetHex) -Profile $sharedTraceProfile
Add-ProfileSection -Lines $lines -Title ("Neighbor Body {0}" -f $neighborBodyProfile.TargetHex) -Profile $neighborBodyProfile

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It compares the two non-instrumented tails, the two trace-heavy siblings, the collapsed shared targets behind them, and the first substantial local neighbor body.',
    'The goal is to decide whether the 0x0001AD7C band really continues through direct tail calls or whether the only remaining substantive body is the adjacent timer/orchestration region.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI transfer tail band assessment written to {0}" -f $resolvedOutputPath)
