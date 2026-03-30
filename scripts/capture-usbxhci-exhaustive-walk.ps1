[CmdletBinding()]
param(
    [uint32[]]$SeedRvas = @([uint32]0x0001BC34),
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
    return Join-Path $outputDirectory 'usbxhci-exhaustive-walk.txt'
}

function Get-DeepOutputPath {
    param([uint32]$TargetRva)

    $outputDirectory = Join-Path (Get-RepoRoot) 'out\dev'
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    return Join-Path $outputDirectory ('usbxhci-exhaustive-{0:X8}-deep.txt' -f $TargetRva)
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

function Join-Strings {
    param(
        [string]$Separator,
        [object[]]$Values
    )

    $filtered = @($Values | Where-Object { $null -ne $_ -and $_ -ne '' } | ForEach-Object { [string]$_ })
    if ($filtered.Count -eq 0) {
        return ''
    }

    return [string]::Join($Separator, $filtered)
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

        if ($line -match '^Target=0x[0-9A-F]+' -or $line -match '^Import=' -or $line -match '^Slot=') {
            $items += $line.Trim()
        }
    }

    return $items
}

function Parse-FunctionBeginRva {
    param([string]$FunctionRange)

    if ($FunctionRange -match '^0x(?<begin>[0-9A-Fa-f]+)-0x(?<end>[0-9A-Fa-f]+)$') {
        return [uint32]('0x' + $matches['begin'])
    }

    throw "Could not parse function range: $FunctionRange"
}

function Parse-InternalTargetRvas {
    param([string[]]$Items)

    $targets = New-Object System.Collections.Generic.List[uint32]
    foreach ($item in $Items) {
        if ($item -match '^Target=0x(?<rva>[0-9A-Fa-f]+) ') {
            $targets.Add([uint32]('0x' + $matches['rva'])) | Out-Null
        }
    }

    return @($targets.ToArray())
}

function Parse-NeighborFunctions {
    param([string[]]$Items)

    $neighbors = New-Object System.Collections.Generic.List[object]
    foreach ($item in $Items) {
        if ($item -match '^Slot=(?<slot>[^|]+) \| Function=0x(?<begin>[0-9A-Fa-f]+)-0x(?<end>[0-9A-Fa-f]+) \| Section=(?<section>[^|]+) \| Size=(?<size>\d+) ') {
            $neighbors.Add([pscustomobject]@{
                Slot     = $matches['slot'].Trim()
                BeginRva = [uint32]('0x' + $matches['begin'])
                EndRva   = [uint32]('0x' + $matches['end'])
                Section  = $matches['section'].Trim()
                Size     = [int]$matches['size']
            }) | Out-Null
        }
    }

    return @($neighbors.ToArray())
}

function Classify-Target {
    param([object]$Profile)

    $imports = Join-Strings -Separator ' | ' -Values $Profile.ImportItems
    $hasTrace = ($imports -match 'WppAutoLogTrace') -or ($imports -match 'DbgPrintEx')
    $hasEtw = $imports -match 'Etw'
    $hasPathStrings = $imports -match 'IoQueryFullDriverPath|RtlInitUnicodeString|RtlUnicodeStringToAnsiString|RtlFreeAnsiString|RtlInitAnsiString'
    $hasIrp = $imports -match 'IoReuseIrp|IoSetCompletionRoutineEx'
    $hasTiming = $imports -match 'KeStallExecutionProcessor|KeQueryTimeIncrement|KeQueryUnbiasedInterruptTime|ExAllocateTimer|ExSetTimer|ExDeleteTimer|KeWaitForSingleObject'

    if ($Profile.Size -le 8 -and $Profile.DirectInternal -eq 0 -and $Profile.DirectIat -eq 0) {
        return [pscustomobject]@{ Kind = 'thunk'; FollowInternal = $false; FollowNeighbors = $false }
    }

    if ($hasTrace -and $Profile.DirectInternal -le 1) {
        return [pscustomobject]@{ Kind = 'trace'; FollowInternal = $false; FollowNeighbors = $false }
    }

    if ($hasEtw -and $Profile.DirectInternal -le 1 -and $Profile.DirectIat -le 2) {
        return [pscustomobject]@{ Kind = 'etw'; FollowInternal = $false; FollowNeighbors = $false }
    }

    if ($hasPathStrings) {
        return [pscustomobject]@{ Kind = 'path-string'; FollowInternal = $false; FollowNeighbors = $false }
    }

    if ($Profile.Size -le 96 -and $Profile.DirectInternal -eq 1 -and $Profile.DirectIat -eq 0) {
        return [pscustomobject]@{ Kind = 'bridge'; FollowInternal = $true; FollowNeighbors = $false }
    }

    if ($Profile.DirectInternal -eq 0 -and $Profile.DirectIat -eq 0) {
        if ($Profile.Size -le 96) {
            return [pscustomobject]@{ Kind = 'stub'; FollowInternal = $false; FollowNeighbors = $false }
        }

        return [pscustomobject]@{ Kind = 'opaque'; FollowInternal = $false; FollowNeighbors = $true }
    }

    if ($hasIrp -or $hasTiming -or ($Profile.Size -ge 256 -and ($Profile.DirectInternal -ge 3 -or $Profile.DirectIat -ge 2))) {
        return [pscustomobject]@{ Kind = 'substantive'; FollowInternal = $true; FollowNeighbors = $true }
    }

    if ($Profile.Size -ge 128 -and ($Profile.DirectInternal -ge 1 -or $Profile.DirectIat -ge 1)) {
        return [pscustomobject]@{ Kind = 'mixed'; FollowInternal = $true; FollowNeighbors = $true }
    }

    return [pscustomobject]@{ Kind = 'mixed'; FollowInternal = $true; FollowNeighbors = $false }
}

function Get-ProfileFromDeepOutput {
    param([string]$Path)

    $content = Get-Content -Path $Path
    $targetHex = Parse-ScalarValue -Content $content -Key 'TargetRva'
    if (-not $targetHex) {
        throw "TargetRva not found in $Path."
    }

    $primary = Parse-PrimaryBlock -Content $content -TargetHex $targetHex
    $internalItems = Parse-SectionItems -Content $content -Header ("## Primary Internal {0}" -f $targetHex)
    $importItems = Parse-SectionItems -Content $content -Header ("## Primary IAT {0}" -f $targetHex)
    $neighborItems = Parse-SectionItems -Content $content -Header ("## Primary Neighbors 0x{0}" -f ('{0:X8}' -f (Parse-FunctionBeginRva -FunctionRange $primary['Function'])))

    return [pscustomobject]@{
        Path           = $Path
        TargetHex      = $targetHex
        Function       = $primary['Function']
        BeginRva       = Parse-FunctionBeginRva -FunctionRange $primary['Function']
        Size           = [int]$primary['Size']
        Section        = $primary['Section']
        DirectInternal = [int]$primary['DirectInternal']
        DirectIat      = [int]$primary['DirectIat']
        InternalItems  = $internalItems
        ImportItems    = $importItems
        NeighborItems  = $neighborItems
        InternalRvas   = Parse-InternalTargetRvas -Items $internalItems
        Neighbors      = Parse-NeighborFunctions -Items $neighborItems
    }
}

function Get-QueueKey {
    param([uint32]$Rva)

    return ('0x{0:X8}' -f $Rva)
}

$resolvedDeepScriptPath = Resolve-DeepScriptPath -RequestedPath $DeepScriptPath
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

if (-not (Test-Path $resolvedDeepScriptPath)) {
    throw "Deep script not found: $resolvedDeepScriptPath"
}

$queue = New-Object System.Collections.Generic.Queue[uint32]
$queuedKeys = @{}
$profilesByKey = @{}
$classCounts = @{}
$visitOrder = New-Object System.Collections.Generic.List[string]

foreach ($seedRva in $SeedRvas) {
    $seedKey = Get-QueueKey -Rva $seedRva
    if (-not $queuedKeys.ContainsKey($seedKey)) {
        $queue.Enqueue($seedRva)
        $queuedKeys[$seedKey] = $true
    }
}

while ($queue.Count -gt 0) {
    $targetRva = $queue.Dequeue()
    $targetKey = Get-QueueKey -Rva $targetRva

    if ($profilesByKey.ContainsKey($targetKey)) {
        continue
    }

    $deepOutputPath = Get-DeepOutputPath -TargetRva $targetRva
    if (-not (Test-Path $deepOutputPath)) {
        & $resolvedDeepScriptPath -TargetRva $targetRva -FollowTargetRvas ([uint32[]]@()) -OutputPath $deepOutputPath *> $null
    }

    $profile = Get-ProfileFromDeepOutput -Path $deepOutputPath
    $canonicalKey = Get-QueueKey -Rva $profile.BeginRva
    if ($profilesByKey.ContainsKey($canonicalKey)) {
        continue
    }

    $classification = Classify-Target -Profile $profile
    $profile | Add-Member -NotePropertyName Classification -NotePropertyValue $classification.Kind
    $profile | Add-Member -NotePropertyName FollowInternal -NotePropertyValue $classification.FollowInternal
    $profile | Add-Member -NotePropertyName FollowNeighbors -NotePropertyValue $classification.FollowNeighbors

    $profilesByKey[$canonicalKey] = $profile
    $visitOrder.Add($canonicalKey) | Out-Null
    if (-not $classCounts.ContainsKey($classification.Kind)) {
        $classCounts[$classification.Kind] = 0
    }
    $classCounts[$classification.Kind]++

    if ($classification.FollowInternal) {
        foreach ($internalRva in $profile.InternalRvas) {
            $internalKey = Get-QueueKey -Rva $internalRva
            if ($internalRva -ne $profile.BeginRva -and -not $profilesByKey.ContainsKey($internalKey) -and -not $queuedKeys.ContainsKey($internalKey)) {
                $queue.Enqueue($internalRva)
                $queuedKeys[$internalKey] = $true
            }
        }
    }

    if ($classification.FollowNeighbors) {
        foreach ($neighbor in $profile.Neighbors) {
            $neighborKey = Get-QueueKey -Rva $neighbor.BeginRva
            if ($neighbor.BeginRva -eq $profile.BeginRva) {
                continue
            }

            if ($neighbor.Size -ge 128 -and -not $profilesByKey.ContainsKey($neighborKey) -and -not $queuedKeys.ContainsKey($neighborKey)) {
                $queue.Enqueue($neighbor.BeginRva)
                $queuedKeys[$neighborKey] = $true
            }
        }
    }
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Exhaustive Walk') | Out-Null
$lines.Add(('Captured: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))) | Out-Null
$lines.Add('') | Out-Null

$seedLabels = @($SeedRvas | ForEach-Object { '0x{0:X8}' -f $_ })
$classLabels = @($classCounts.GetEnumerator() | Sort-Object Name | ForEach-Object { '{0}={1}' -f $_.Key, $_.Value })
$seedSummary = if ($seedLabels.Count -gt 0) { Join-Strings -Separator ', ' -Values $seedLabels } else { '<none>' }
$classSummary = if ($classLabels.Count -gt 0) { Join-Strings -Separator ', ' -Values $classLabels } else { '<none>' }

Add-Section -Lines $lines -Title 'Summary' -Body @(
    ('Seeds           : {0}' -f $seedSummary),
    ('VisitedTargets  : {0}' -f $visitOrder.Count),
    ('ClassBreakdown  : {0}' -f $classSummary),
    'TraversalMode   : direct internal targets from substantive/bridge nodes plus substantial same-band neighbors'
)

foreach ($key in $visitOrder) {
    $profile = $profilesByKey[$key]
    $body = @(
        ('Function       : {0}' -f $profile.Function),
        ('Path           : {0}' -f $profile.Path),
        ('Class          : {0}' -f $profile.Classification),
        ('Size           : {0}' -f $profile.Size),
        ('DirectInternal : {0}' -f $profile.DirectInternal),
        ('DirectIat      : {0}' -f $profile.DirectIat)
    )

    if ($profile.InternalItems.Count -gt 0) {
        $body += 'InternalTargets:'
        $body += ($profile.InternalItems | ForEach-Object { "  $_" })
    }

    if ($profile.ImportItems.Count -gt 0) {
        $body += 'DirectImports:'
        $body += ($profile.ImportItems | ForEach-Object { "  $_" })
    }

    if ($profile.Neighbors.Count -gt 0) {
        $body += 'SubstantialNeighbors:'
        $body += (
            $profile.Neighbors |
            Where-Object { $_.BeginRva -ne $profile.BeginRva -and $_.Size -ge 128 } |
            ForEach-Object { "  Slot=$($_.Slot) | Function=0x$('{0:X8}' -f $_.BeginRva)-0x$('{0:X8}' -f $_.EndRva) | Section=$($_.Section) | Size=$($_.Size)" }
        )
    }

    Add-Section -Lines $lines -Title ("Target {0}" -f $key) -Body $body
}

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host ("USBXHCI exhaustive walk written to {0}" -f $resolvedOutputPath)
