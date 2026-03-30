[CmdletBinding()]
param(
    [string]$PrevPath,
    [string]$NextPath,
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
    return Join-Path $outputDirectory 'usbxhci-endpoint-neighbor-compare.txt'
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

function Parse-TargetBlock {
    param(
        [string[]]$Content,
        [uint32]$TargetRva
    )

    $header = ('## Primary Target 0x{0:X8}' -f $TargetRva)
    $startIndex = [Array]::IndexOf($Content, $header)
    if ($startIndex -lt 0) {
        throw "Primary block not found for target 0x$('{0:X8}' -f $TargetRva)."
    }

    $lines = New-Object System.Collections.Generic.List[string]
    for ($index = $startIndex + 1; $index -lt $Content.Length; $index++) {
        $line = $Content[$index]
        if ($line -like '## *') {
            break
        }

        if ($line -ne '') {
            $lines.Add($line) | Out-Null
        }
    }

    $block = [ordered]@{}
    foreach ($line in $lines) {
        if ($line -match '^\s*([^:]+?)\s*:\s*(.+)$') {
            $block[$matches[1].Trim()] = $matches[2].Trim()
        }
    }

    return $block
}

function Parse-InternalCalls {
    param(
        [string[]]$Content,
        [uint32]$TargetRva
    )

    $header = ('## Primary Internal 0x{0:X8}' -f $TargetRva)
    $startIndex = [Array]::IndexOf($Content, $header)
    if ($startIndex -lt 0) {
        throw "Internal-call block not found for target 0x$('{0:X8}' -f $TargetRva)."
    }

    $results = @()
    for ($index = $startIndex + 1; $index -lt $Content.Length; $index++) {
        $line = $Content[$index]
        if ($line -like '## *') {
            break
        }

        if ($line -match '^Target=0x([0-9A-F]+) \| Calls=(\d+) \| TargetSection=([^|]+) \| SampleSite=0x([0-9A-F]+) \| Bytes=(.+)$') {
            $results += [pscustomobject]@{
                TargetRva    = [uint32]("0x{0}" -f $matches[1])
                Calls        = [int]$matches[2]
                TargetSection = $matches[3].Trim()
                SampleSite   = [uint32]("0x{0}" -f $matches[4])
                Bytes        = $matches[5].Trim()
            }
        }
    }

    return $results
}

function Parse-PrimaryTargetRva {
    param([string[]]$Content)

    foreach ($line in $Content) {
        if ($line -match '^TargetRva\s+:\s+0x([0-9A-F]+)$') {
            return [uint32]("0x{0}" -f $matches[1])
        }
    }

    throw 'TargetRva summary line not found.'
}

$resolvedPrevPath = Resolve-InputPath -RequestedPath $PrevPath -DefaultName 'usbxhci-endpoint-prev-deep.txt'
$resolvedNextPath = Resolve-InputPath -RequestedPath $NextPath -DefaultName 'usbxhci-endpoint-next-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$prevContent = Get-Content -Path $resolvedPrevPath
$nextContent = Get-Content -Path $resolvedNextPath

$prevTargetRva = Parse-PrimaryTargetRva -Content $prevContent
$nextTargetRva = Parse-PrimaryTargetRva -Content $nextContent

$prevBlock = Parse-TargetBlock -Content $prevContent -TargetRva $prevTargetRva
$nextBlock = Parse-TargetBlock -Content $nextContent -TargetRva $nextTargetRva

$prevCalls = Parse-InternalCalls -Content $prevContent -TargetRva $prevTargetRva
$nextCalls = Parse-InternalCalls -Content $nextContent -TargetRva $nextTargetRva

$prevMap = @{}
foreach ($call in $prevCalls) {
    $prevMap[$call.TargetRva] = $call
}

$nextMap = @{}
foreach ($call in $nextCalls) {
    $nextMap[$call.TargetRva] = $call
}

$sharedTargets = @($prevMap.Keys | Where-Object { $nextMap.ContainsKey($_) } | Sort-Object)
$prevOnlyTargets = @($prevMap.Keys | Where-Object { -not $nextMap.ContainsKey($_) } | Sort-Object)
$nextOnlyTargets = @($nextMap.Keys | Where-Object { -not $prevMap.ContainsKey($_) } | Sort-Object)

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Endpoint Neighbor Compare') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('PrevPath          : {0}' -f $resolvedPrevPath),
    ('NextPath          : {0}' -f $resolvedNextPath),
    ('PrevTarget        : 0x{0:X8}' -f $prevTargetRva),
    ('NextTarget        : 0x{0:X8}' -f $nextTargetRva),
    ('SharedTargets     : {0}' -f $sharedTargets.Count),
    ('PrevOnlyTargets   : {0}' -f $prevOnlyTargets.Count),
    ('NextOnlyTargets   : {0}' -f $nextOnlyTargets.Count)
)

Add-Section -Lines $lines -Title ('Primary {0:X8}' -f $prevTargetRva) -Body @(
    ('Function     : {0}' -f $prevBlock['Function']),
    ('Size         : {0}' -f $prevBlock['Size']),
    ('DirectInternal: {0}' -f $prevBlock['DirectInternal']),
    ('DirectIat     : {0}' -f $prevBlock['DirectIat'])
)

Add-Section -Lines $lines -Title ('Primary {0:X8}' -f $nextTargetRva) -Body @(
    ('Function     : {0}' -f $nextBlock['Function']),
    ('Size         : {0}' -f $nextBlock['Size']),
    ('DirectInternal: {0}' -f $nextBlock['DirectInternal']),
    ('DirectIat     : {0}' -f $nextBlock['DirectIat'])
)

$sharedBody = @()
if ($sharedTargets.Count -eq 0) {
    $sharedBody += 'No shared direct internal targets.'
}
else {
    foreach ($target in $sharedTargets) {
        $prevCall = $prevMap[$target]
        $nextCall = $nextMap[$target]
        $sharedBody += ('Target=0x{0:X8} | PrevCalls={1} | NextCalls={2} | Section={3}' -f $target, $prevCall.Calls, $nextCall.Calls, $prevCall.TargetSection)
    }
}
Add-Section -Lines $lines -Title 'Shared Direct Internal Targets' -Body $sharedBody

$prevOnlyBody = @()
if ($prevOnlyTargets.Count -eq 0) {
    $prevOnlyBody += 'No prev-only direct internal targets.'
}
else {
    foreach ($target in $prevOnlyTargets) {
        $call = $prevMap[$target]
        $prevOnlyBody += ('Target=0x{0:X8} | Calls={1} | Section={2}' -f $target, $call.Calls, $call.TargetSection)
    }
}
Add-Section -Lines $lines -Title ('Prev-Only Targets 0x{0:X8}' -f $prevTargetRva) -Body $prevOnlyBody

$nextOnlyBody = @()
if ($nextOnlyTargets.Count -eq 0) {
    $nextOnlyBody += 'No next-only direct internal targets.'
}
else {
    foreach ($target in $nextOnlyTargets) {
        $call = $nextMap[$target]
        $nextOnlyBody += ('Target=0x{0:X8} | Calls={1} | Section={2}' -f $target, $call.Calls, $call.TargetSection)
    }
}
Add-Section -Lines $lines -Title ('Next-Only Targets 0x{0:X8}' -f $nextTargetRva) -Body $nextOnlyBody

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only and compares the two endpoint-neighbor deep captures around 0x00008454.',
    'The goal is to identify which direct internal targets are common between the two neighboring endpoint bodies and which targets are unique to each side.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI endpoint neighbor comparison written to {0}" -f $resolvedOutputPath)
