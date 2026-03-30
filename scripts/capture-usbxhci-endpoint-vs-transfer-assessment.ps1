[CmdletBinding()]
param(
    [string]$EndpointPath,
    [string]$TransferPath,
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
    return Join-Path $outputDirectory 'usbxhci-endpoint-vs-transfer-assessment.txt'
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

function Get-Profile {
    param([string]$Path)

    $content = Get-Content -Path $Path
    $targetHex = Parse-ScalarValue -Content $content -Key 'TargetRva'
    if (-not $targetHex) {
        throw "TargetRva not found in $Path."
    }

    $primary = Parse-PrimaryBlock -Content $content -TargetRvaHex $targetHex
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

$resolvedEndpointPath = Resolve-InputPath -RequestedPath $EndpointPath -DefaultName 'usbxhci-endpoint-target-deep.txt'
$resolvedTransferPath = Resolve-InputPath -RequestedPath $TransferPath -DefaultName 'usbxhci-transfer-077FC-deep.txt'
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

$endpointProfile = Get-Profile -Path $resolvedEndpointPath
$transferProfile = Get-Profile -Path $resolvedTransferPath

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Endpoint vs Transfer Assessment') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('EndpointTarget  : {0}' -f $endpointProfile.TargetHex),
    ('TransferTarget  : {0}' -f $transferProfile.TargetHex),
    'RecommendedNext : 0x000077FC'
)

$endpointBody = @(
    ('Path          : {0}' -f $endpointProfile.Path),
    ('Function      : {0}' -f $endpointProfile.Function),
    ('Size          : {0}' -f $endpointProfile.Size),
    ('DirectInternal: {0}' -f $endpointProfile.DirectInternal),
    ('DirectIat     : {0}' -f $endpointProfile.DirectIat),
    'Role          : endpoint-side wrapper/continuation'
)
if ($endpointProfile.InternalItems.Count -gt 0) {
    $endpointBody += 'InternalTargets:'
    $endpointBody += ($endpointProfile.InternalItems | ForEach-Object { "  $_" })
}
if ($endpointProfile.ImportItems.Count -gt 0) {
    $endpointBody += 'DirectImports:'
    $endpointBody += ($endpointProfile.ImportItems | ForEach-Object { "  $_" })
}
Add-Section -Lines $lines -Title ("Endpoint Candidate {0}" -f $endpointProfile.TargetHex) -Body $endpointBody

$transferBody = @(
    ('Path          : {0}' -f $transferProfile.Path),
    ('Function      : {0}' -f $transferProfile.Function),
    ('Size          : {0}' -f $transferProfile.Size),
    ('DirectInternal: {0}' -f $transferProfile.DirectInternal),
    ('DirectIat     : {0}' -f $transferProfile.DirectIat),
    'Role          : transfer-side hot cluster'
)
if ($transferProfile.InternalItems.Count -gt 0) {
    $transferBody += 'InternalTargets:'
    $transferBody += ($transferProfile.InternalItems | ForEach-Object { "  $_" })
}
if ($transferProfile.ImportItems.Count -gt 0) {
    $transferBody += 'DirectImports:'
    $transferBody += ($transferProfile.ImportItems | ForEach-Object { "  $_" })
}
Add-Section -Lines $lines -Title ("Transfer Candidate {0}" -f $transferProfile.TargetHex) -Body $transferBody

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only. It compares the endpoint-side continuation at 0x00008454 against the transfer-side cluster at 0x000077FC.',
    'The goal is to decide which side is the richer next reverse-engineering target for polling/scheduling behavior.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI endpoint vs transfer assessment written to {0}" -f $resolvedOutputPath)
