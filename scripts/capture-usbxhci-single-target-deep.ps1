[CmdletBinding()]
param(
    [string]$ImagePath,
    [string]$ClusterProfilePath,
    [string]$FeatureMapPath,
    [uint32]$TargetRva = [uint32]0x00006E74,
    [uint32[]]$FollowTargetRvas = @(
        [uint32]0x00008454,
        [uint32]0x00054F74
    ),
    [int]$NeighborRadius = 2,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-ImagePath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return (Join-Path $env:SystemRoot 'System32\drivers\USBXHCI.SYS')
}

function Resolve-ClusterProfilePath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return (Join-Path (Join-Path (Get-RepoRoot) 'out\dev') 'usbxhci-cluster-profile.txt')
}

function Resolve-FeatureMapPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return (Join-Path (Join-Path (Get-RepoRoot) 'out\dev') 'usbxhci-feature-map.txt')
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    $defaultDir = Join-Path (Get-RepoRoot) 'out\dev'
    New-Item -ItemType Directory -Force -Path $defaultDir | Out-Null
    return (Join-Path $defaultDir 'usbxhci-single-target-deep.txt')
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

function Read-UInt16LE {
    param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToUInt16($Bytes, $Offset)
}

function Read-UInt32LE {
    param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToUInt32($Bytes, $Offset)
}

function Read-UInt64LE {
    param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToUInt64($Bytes, $Offset)
}

function Read-Int32LE {
    param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToInt32($Bytes, $Offset)
}

function Read-AsciiZ {
    param([byte[]]$Bytes, [int]$Offset)

    if ($Offset -lt 0 -or $Offset -ge $Bytes.Length) {
        return $null
    }

    $end = $Offset
    while ($end -lt $Bytes.Length -and $Bytes[$end] -ne 0) {
        $end++
    }

    if ($end -le $Offset) {
        return ''
    }

    return [System.Text.Encoding]::ASCII.GetString($Bytes, $Offset, $end - $Offset)
}

function Format-HexBytes {
    param(
        [byte[]]$Bytes,
        [int]$Offset,
        [int]$Count
    )

    $available = $Bytes.Length - $Offset
    $actualCount = [Math]::Min($Count, $available)
    if ($actualCount -le 0) {
        return ''
    }

    $slice = New-Object byte[] $actualCount
    [Buffer]::BlockCopy($Bytes, $Offset, $slice, 0, $actualCount)
    return (($slice | ForEach-Object { $_.ToString('X2') }) -join ' ')
}

function Convert-RvaToOffset {
    param(
        [uint32]$Rva,
        [object[]]$Sections,
        [uint32]$SizeOfHeaders
    )

    if ($Rva -lt $SizeOfHeaders) {
        return [int]$Rva
    }

    foreach ($section in $Sections) {
        $sectionSpan = [Math]::Max([uint32]$section.VirtualSize, [uint32]$section.SizeOfRawData)
        if ($Rva -ge $section.VirtualAddress -and $Rva -lt ($section.VirtualAddress + $sectionSpan)) {
            return [int]($section.PointerToRawData + ($Rva - $section.VirtualAddress))
        }
    }

    return $null
}

function Convert-TimeDateStamp {
    param([uint32]$TimeDateStamp)
    return ([DateTimeOffset]::FromUnixTimeSeconds([int64]$TimeDateStamp)).ToString('yyyy-MM-dd HH:mm:ss zzz')
}

function Get-SectionNameForRva {
    param(
        [uint32]$Rva,
        [object[]]$Sections
    )

    foreach ($section in $Sections) {
        $sectionSpan = [Math]::Max([uint32]$section.VirtualSize, [uint32]$section.SizeOfRawData)
        if ($Rva -ge $section.VirtualAddress -and $Rva -lt ($section.VirtualAddress + $sectionSpan)) {
            return $section.Name
        }
    }

    return '<unmapped>'
}

function Parse-PeLayout {
    param([byte[]]$Bytes)

    if ([System.Text.Encoding]::ASCII.GetString($Bytes, 0, 2) -ne 'MZ') {
        throw 'DOS header signature mismatch.'
    }

    $peOffset = [int](Read-UInt32LE -Bytes $Bytes -Offset 0x3C)
    if ([System.Text.Encoding]::ASCII.GetString($Bytes, $peOffset, 4) -ne ('PE' + [char]0 + [char]0)) {
        throw 'PE signature mismatch.'
    }

    $coffOffset = $peOffset + 4
    $sectionCount = Read-UInt16LE -Bytes $Bytes -Offset ($coffOffset + 2)
    $timeDateStamp = Read-UInt32LE -Bytes $Bytes -Offset ($coffOffset + 4)
    $sizeOfOptionalHeader = Read-UInt16LE -Bytes $Bytes -Offset ($coffOffset + 16)
    $optionalHeaderOffset = $coffOffset + 20
    $magic = Read-UInt16LE -Bytes $Bytes -Offset $optionalHeaderOffset
    $isPe32Plus = ($magic -eq 0x20B)
    if (-not $isPe32Plus -and $magic -ne 0x10B) {
        throw ('Unsupported optional header magic 0x{0:X4}' -f $magic)
    }

    $entryPointRva = Read-UInt32LE -Bytes $Bytes -Offset ($optionalHeaderOffset + 16)
    $sizeOfHeaders = Read-UInt32LE -Bytes $Bytes -Offset ($optionalHeaderOffset + 60)
    $numberOfRvaAndSizes = Read-UInt32LE -Bytes $Bytes -Offset ($optionalHeaderOffset + 108)
    $dataDirectoryBaseOffset = if ($isPe32Plus) { 112 } else { 96 }
    $dataDirectoryOffset = $optionalHeaderOffset + $dataDirectoryBaseOffset

    $dataDirectories = @()
    $directoryCount = [Math]::Min([int]$numberOfRvaAndSizes, 16)
    for ($index = 0; $index -lt $directoryCount; $index++) {
        $directoryOffset = $dataDirectoryOffset + ($index * 8)
        $dataDirectories += [pscustomobject]@{
            Index = $index
            RVA   = Read-UInt32LE -Bytes $Bytes -Offset $directoryOffset
            Size  = Read-UInt32LE -Bytes $Bytes -Offset ($directoryOffset + 4)
        }
    }

    $sectionTableOffset = $optionalHeaderOffset + $sizeOfOptionalHeader
    $sections = @()
    for ($index = 0; $index -lt $sectionCount; $index++) {
        $sectionOffset = $sectionTableOffset + ($index * 40)
        $nameBytes = New-Object byte[] 8
        [Buffer]::BlockCopy($Bytes, $sectionOffset, $nameBytes, 0, 8)
        $nullIndex = [Array]::IndexOf($nameBytes, [byte]0)
        if ($nullIndex -lt 0) {
            $nullIndex = $nameBytes.Length
        }

        $sections += [pscustomobject]@{
            Name             = [System.Text.Encoding]::ASCII.GetString($nameBytes, 0, $nullIndex)
            VirtualSize      = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 8)
            VirtualAddress   = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 12)
            SizeOfRawData    = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 16)
            PointerToRawData = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 20)
        }
    }

    return [pscustomobject]@{
        TimeDateStamp   = $timeDateStamp
        EntryPointRva   = $entryPointRva
        SizeOfHeaders   = $sizeOfHeaders
        DataDirectories = $dataDirectories
        Sections        = $sections
    }
}

function Get-DataDirectory {
    param(
        [object]$Layout,
        [int]$Index
    )

    return ($Layout.DataDirectories | Where-Object { $_.Index -eq $Index } | Select-Object -First 1)
}

function Parse-RuntimeFunctions {
    param(
        [byte[]]$Bytes,
        [object]$Layout
    )

    $directory = Get-DataDirectory -Layout $Layout -Index 3
    if (-not $directory -or $directory.RVA -eq 0 -or $directory.Size -eq 0) {
        return @()
    }

    $offset = Convert-RvaToOffset -Rva $directory.RVA -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
    if ($null -eq $offset) {
        return @()
    }

    $functions = @()
    $count = [int]($directory.Size / 12)
    for ($index = 0; $index -lt $count; $index++) {
        $entryOffset = $offset + ($index * 12)
        if (($entryOffset + 12) -gt $Bytes.Length) {
            break
        }

        $beginRva = Read-UInt32LE -Bytes $Bytes -Offset $entryOffset
        $endRva = Read-UInt32LE -Bytes $Bytes -Offset ($entryOffset + 4)
        $unwindRva = Read-UInt32LE -Bytes $Bytes -Offset ($entryOffset + 8)
        if ($beginRva -eq 0 -and $endRva -eq 0) {
            continue
        }

        $sectionName = Get-SectionNameForRva -Rva $beginRva -Sections $Layout.Sections
        if ($sectionName -eq '<unmapped>') {
            continue
        }

        $functions += [pscustomobject]@{
            BeginRva  = $beginRva
            EndRva    = $endRva
            Size      = ($endRva - $beginRva)
            UnwindRva = $unwindRva
            Section   = $sectionName
        }
    }

    return ($functions | Sort-Object BeginRva)
}

function Parse-ImportsWithIat {
    param(
        [byte[]]$Bytes,
        [object]$Layout
    )

    $directory = Get-DataDirectory -Layout $Layout -Index 1
    if (-not $directory -or $directory.RVA -eq 0 -or $directory.Size -eq 0) {
        return @()
    }

    $descriptorOffset = Convert-RvaToOffset -Rva $directory.RVA -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
    if ($null -eq $descriptorOffset) {
        return @()
    }

    $imports = @()
    $thunkSize = 8
    $ordinalMask = ([uint64]1 -shl 63)
    for ($descriptorIndex = 0; ; $descriptorIndex++) {
        $offset = $descriptorOffset + ($descriptorIndex * 20)
        if (($offset + 20) -gt $Bytes.Length) {
            break
        }

        $originalFirstThunk = Read-UInt32LE -Bytes $Bytes -Offset $offset
        $nameRva = Read-UInt32LE -Bytes $Bytes -Offset ($offset + 12)
        $firstThunk = Read-UInt32LE -Bytes $Bytes -Offset ($offset + 16)
        if (($originalFirstThunk -bor $nameRva -bor $firstThunk) -eq 0) {
            break
        }

        $nameOffset = Convert-RvaToOffset -Rva $nameRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
        if ($null -eq $nameOffset) {
            continue
        }

        $dllName = Read-AsciiZ -Bytes $Bytes -Offset $nameOffset
        $lookupRva = if ($originalFirstThunk -ne 0) { $originalFirstThunk } else { $firstThunk }
        $lookupOffset = Convert-RvaToOffset -Rva $lookupRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
        if ($null -eq $lookupOffset) {
            continue
        }

        for ($thunkIndex = 0; ; $thunkIndex++) {
            $lookupEntryOffset = $lookupOffset + ($thunkIndex * $thunkSize)
            if (($lookupEntryOffset + $thunkSize) -gt $Bytes.Length) {
                break
            }

            $lookupValue = Read-UInt64LE -Bytes $Bytes -Offset $lookupEntryOffset
            if ($lookupValue -eq 0) {
                break
            }

            if (($lookupValue -band $ordinalMask) -ne 0) {
                continue
            }

            $hintNameRva = [uint32]$lookupValue
            $hintNameOffset = Convert-RvaToOffset -Rva $hintNameRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
            if ($null -eq $hintNameOffset) {
                continue
            }

            $imports += [pscustomobject]@{
                DllName    = $dllName
                ImportName = Read-AsciiZ -Bytes $Bytes -Offset ($hintNameOffset + 2)
                IatRva     = [uint32]($firstThunk + ($thunkIndex * $thunkSize))
            }
        }
    }

    return $imports
}

function Parse-UnwindInfoSummary {
    param(
        [byte[]]$Bytes,
        [object]$Layout,
        [uint32]$UnwindRva
    )

    if ($UnwindRva -eq 0) {
        return '<none>'
    }

    $offset = Convert-RvaToOffset -Rva $UnwindRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
    if ($null -eq $offset -or ($offset + 4) -gt $Bytes.Length) {
        return '<unmapped>'
    }

    $versionAndFlags = $Bytes[$offset]
    $version = ($versionAndFlags -band 0x7)
    $flags = ($versionAndFlags -shr 3)
    $prologSize = $Bytes[$offset + 1]
    $codeCount = $Bytes[$offset + 2]
    $frameByte = $Bytes[$offset + 3]
    $frameRegister = ($frameByte -band 0x0F)
    $frameOffset = ($frameByte -shr 4)

    return "Ver=$version Flags=0x$('{0:X2}' -f $flags) Prolog=$prologSize Codes=$codeCount FrameReg=$frameRegister FrameOff=$frameOffset"
}

function Parse-ClusterProfileCandidates {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Cluster profile not found: $Path"
    }

    $candidates = New-Object System.Collections.Generic.List[object]
    $currentFeature = $null
    foreach ($line in Get-Content $Path) {
        if ($line -match '^## (?<feature>.+) Candidate Profiles$') {
            $currentFeature = $matches['feature']
            continue
        }

        if ($line -match '^## ') {
            $currentFeature = $null
            continue
        }

        if (-not $currentFeature) {
            continue
        }

        if ($line -match '^Function=0x(?<begin>[0-9A-Fa-f]+)-0x(?<end>[0-9A-Fa-f]+) \| Section=(?<section>[^|]+) \| RefCount=(?<ref>\d+) \| IatHits=(?<iat>\d+) \| Unwind=0x(?<unwind>[0-9A-Fa-f]+) \| UnwindInfo=(?<unwindInfo>[^|]+) \| Imports=(?<imports>[^|]+) \| Matches=(?<matches>.+)$') {
            $imports = @()
            if ($matches['imports'].Trim() -ne '<none>') {
                $imports = @($matches['imports'].Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
            }

            $candidates.Add([pscustomobject]@{
                Feature     = $currentFeature
                BeginRva    = [uint32]('0x' + $matches['begin'])
                EndRva      = [uint32]('0x' + $matches['end'])
                Section     = $matches['section'].Trim()
                Imports     = $imports
                Matches     = $matches['matches'].Trim()
                IatHits     = [int]$matches['iat']
                UnwindInfo  = $matches['unwindInfo'].Trim()
            }) | Out-Null
        }
    }

    return @($candidates.ToArray())
}

function Parse-FeatureSummaryBands {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Feature map not found: $Path"
    }

    $bands = New-Object System.Collections.Generic.List[object]
    foreach ($line in Get-Content $Path) {
        if ($line -match '^(?<feature>\w+) \| Anchors=\d+ \| Refs=\d+ \| Functions=\d+ \| AnchorBand=0x[0-9A-Fa-f]+ .. 0x[0-9A-Fa-f]+ \| FunctionBand=0x(?<begin>[0-9A-Fa-f]+) .. 0x(?<end>[0-9A-Fa-f]+)$') {
            $bands.Add([pscustomobject]@{
                Feature  = $matches['feature']
                BeginRva = [uint32]('0x' + $matches['begin'])
                EndRva   = [uint32]('0x' + $matches['end'])
            }) | Out-Null
        }
    }

    return @($bands.ToArray())
}

function Find-ContainingFunction {
    param(
        [uint32]$CodeRva,
        [object[]]$Functions
    )

    foreach ($function in $Functions) {
        if ($CodeRva -ge $function.BeginRva -and $CodeRva -lt $function.EndRva) {
            return $function
        }
    }

    return $null
}

function Find-CandidateByRva {
    param(
        [uint32]$Rva,
        [object[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if ($Rva -ge $candidate.BeginRva -and $Rva -lt $candidate.EndRva) {
            return $candidate
        }
    }

    return $null
}

function Find-NearestCandidates {
    param(
        [uint32]$Rva,
        [object[]]$Candidates,
        [int]$Count = 4
    )

    return @(
        $Candidates |
        Sort-Object -Property @{ Expression = { [Math]::Min([Math]::Abs([int64]$Rva - [int64]$_.BeginRva), [Math]::Abs([int64]$Rva - [int64]$_.EndRva)) }; Ascending = $true }, 'BeginRva' |
        Select-Object -First $Count
    )
}

function Get-FeatureBandHits {
    param(
        [uint32]$Rva,
        [object[]]$Bands
    )

    return @($Bands | Where-Object { $Rva -ge $_.BeginRva -and $Rva -le $_.EndRva })
}

function Get-NeighborFunctions {
    param(
        [object]$Function,
        [object[]]$Functions,
        [int]$Radius
    )

    $index = -1
    for ($i = 0; $i -lt $Functions.Count; $i++) {
        if ($Functions[$i].BeginRva -eq $Function.BeginRva) {
            $index = $i
            break
        }
    }

    if ($index -lt 0) {
        return @()
    }

    $start = [Math]::Max(0, $index - $Radius)
    $end = [Math]::Min($Functions.Count - 1, $index + $Radius)
    $neighbors = New-Object System.Collections.Generic.List[object]
    for ($i = $start; $i -le $end; $i++) {
        $neighbors.Add([pscustomobject]@{
            RelativeIndex = ($i - $index)
            Function      = $Functions[$i]
        }) | Out-Null
    }

    return @($neighbors.ToArray())
}

function Find-DirectCallSitesForFunction {
    param(
        [byte[]]$Bytes,
        [object]$Layout,
        [object]$Function,
        [object[]]$Functions,
        [object[]]$Imports
    )

    $importsByIatRva = @{}
    foreach ($import in $Imports) {
        $importsByIatRva[[uint32]$import.IatRva] = $import
    }

    $sites = New-Object System.Collections.Generic.List[object]
    $offset = Convert-RvaToOffset -Rva $Function.BeginRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
    if ($null -eq $offset) {
        return @()
    }

    $length = [int]($Function.EndRva - $Function.BeginRva)
    $endOffset = [Math]::Min($offset + $length, $Bytes.Length)
    for ($cursor = $offset; $cursor -lt $endOffset; $cursor++) {
        $instructionRva = [uint32]($Function.BeginRva + ($cursor - $offset))

        if (($cursor + 5) -le $endOffset -and $Bytes[$cursor] -eq 0xE8) {
            $disp = Read-Int32LE -Bytes $Bytes -Offset ($cursor + 1)
            $nextRva = [uint32]($instructionRva + 5)
            $targetRva64 = [int64]$nextRva + [int64]$disp
            if ($targetRva64 -lt 0 -or $targetRva64 -gt [uint32]::MaxValue) {
                continue
            }

            $calleeRva = [uint32]$targetRva64
            $calleeFunction = Find-ContainingFunction -CodeRva $calleeRva -Functions $Functions
            if (-not $calleeFunction) {
                continue
            }

            $sites.Add([pscustomobject]@{
                InstructionKind   = 'CALL_REL'
                InstructionRva    = $instructionRva
                InstructionHex    = Format-HexBytes -Bytes $Bytes -Offset $cursor -Count 5
                TargetFunctionRva = $calleeFunction.BeginRva
                TargetFunctionEnd = $calleeFunction.EndRva
                TargetSection     = $calleeFunction.Section
                TargetLabel       = ('0x{0:X8}' -f $calleeFunction.BeginRva)
                ImportName        = $null
            }) | Out-Null
            continue
        }

        if (($cursor + 6) -le $endOffset -and $Bytes[$cursor] -eq 0xFF -and ($Bytes[$cursor + 1] -eq 0x15 -or $Bytes[$cursor + 1] -eq 0x25)) {
            $disp = Read-Int32LE -Bytes $Bytes -Offset ($cursor + 2)
            $nextRva = [uint32]($instructionRva + 6)
            $iatRva64 = [int64]$nextRva + [int64]$disp
            if ($iatRva64 -lt 0 -or $iatRva64 -gt [uint32]::MaxValue) {
                continue
            }

            $import = $importsByIatRva[[uint32]$iatRva64]
            if (-not $import) {
                continue
            }

            $sites.Add([pscustomobject]@{
                InstructionKind   = if ($Bytes[$cursor + 1] -eq 0x15) { 'CALL_IAT' } else { 'JMP_IAT' }
                InstructionRva    = $instructionRva
                InstructionHex    = Format-HexBytes -Bytes $Bytes -Offset $cursor -Count 6
                TargetFunctionRva = $null
                TargetFunctionEnd = $null
                TargetSection     = '.idata'
                TargetLabel       = ('{0}!{1}' -f $import.DllName, $import.ImportName)
                ImportName        = ('{0}!{1}' -f $import.DllName, $import.ImportName)
            }) | Out-Null
        }
    }

    return @($sites | Sort-Object InstructionRva)
}

function Format-FunctionLabel {
    param([object]$Function)
    return ('0x{0:X8}-0x{1:X8}' -f $Function.BeginRva, $Function.EndRva)
}

$resolvedImagePath = Resolve-ImagePath -RequestedPath $ImagePath
$resolvedClusterProfilePath = Resolve-ClusterProfilePath -RequestedPath $ClusterProfilePath
$resolvedFeatureMapPath = Resolve-FeatureMapPath -RequestedPath $FeatureMapPath
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath

if (-not (Test-Path $resolvedImagePath)) {
    throw "USBXHCI image not found: $resolvedImagePath"
}

$outputDir = Split-Path -Parent $resolvedOutputPath
if ($outputDir) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

$bytes = [System.IO.File]::ReadAllBytes($resolvedImagePath)
$file = Get-Item $resolvedImagePath
$layout = Parse-PeLayout -Bytes $bytes
$functions = Parse-RuntimeFunctions -Bytes $bytes -Layout $layout
$imports = Parse-ImportsWithIat -Bytes $bytes -Layout $layout
$clusterCandidates = Parse-ClusterProfileCandidates -Path $resolvedClusterProfilePath
$featureBands = Parse-FeatureSummaryBands -Path $resolvedFeatureMapPath

$primaryFunction = Find-ContainingFunction -CodeRva $TargetRva -Functions $functions
if (-not $primaryFunction) {
    throw ('Could not resolve target RVA 0x{0:X8} to a runtime function.' -f $TargetRva)
}

$primaryNeighbors = Get-NeighborFunctions -Function $primaryFunction -Functions $functions -Radius $NeighborRadius
$primaryCalls = Find-DirectCallSitesForFunction -Bytes $bytes -Layout $layout -Function $primaryFunction -Functions $functions -Imports $imports
$primaryInternal = @($primaryCalls | Where-Object { $_.InstructionKind -eq 'CALL_REL' })
$primaryIat = @($primaryCalls | Where-Object { $_.InstructionKind -ne 'CALL_REL' })

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Single-Target Deep Recon') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "ImagePath          : $resolvedImagePath",
    "FileVersion        : $($file.VersionInfo.FileVersion)",
    "ProductVersion     : $($file.VersionInfo.ProductVersion)",
    "TimeDateStamp      : 0x$('{0:X8}' -f $layout.TimeDateStamp) ($(Convert-TimeDateStamp -TimeDateStamp $layout.TimeDateStamp))",
    "ClusterProfilePath : $resolvedClusterProfilePath",
    "FeatureMapPath     : $resolvedFeatureMapPath",
    "TargetRva          : 0x$('{0:X8}' -f $TargetRva)",
    "PrimaryFunction    : $(Format-FunctionLabel -Function $primaryFunction)",
    "FollowTargets      : $([string]::Join(', ', ($FollowTargetRvas | ForEach-Object { '0x{0:X8}' -f $_ })))"
)

$primaryUnwind = Parse-UnwindInfoSummary -Bytes $bytes -Layout $layout -UnwindRva $primaryFunction.UnwindRva
Add-Section -Lines $lines -Title ("Primary Target 0x{0:X8}" -f $TargetRva) -Body @(
    "Function           : $(Format-FunctionLabel -Function $primaryFunction)",
    "Section            : $($primaryFunction.Section)",
    "Size               : $($primaryFunction.Size)",
    "UnwindRva          : 0x$('{0:X8}' -f $primaryFunction.UnwindRva)",
    "UnwindInfo         : $primaryUnwind",
    "DirectInternal     : $($primaryInternal.Count)",
    "DirectIat          : $($primaryIat.Count)"
)

$neighborLines = @(
    $primaryNeighbors | ForEach-Object {
        $neighbor = $_.Function
        $neighborUnwind = Parse-UnwindInfoSummary -Bytes $bytes -Layout $layout -UnwindRva $neighbor.UnwindRva
        $marker = if ($_.RelativeIndex -eq 0) { '<primary>' } elseif ($_.RelativeIndex -lt 0) { ('prev{0}' -f ([Math]::Abs($_.RelativeIndex))) } else { ('next{0}' -f $_.RelativeIndex) }
        "Slot=$marker | Function=$(Format-FunctionLabel -Function $neighbor) | Section=$($neighbor.Section) | Size=$($neighbor.Size) | Unwind=0x$('{0:X8}' -f $neighbor.UnwindRva) | UnwindInfo=$neighborUnwind"
    }
)
Add-Section -Lines $lines -Title ("Primary Neighbors 0x{0:X8}" -f $primaryFunction.BeginRva) -Body $neighborLines

$primaryInternalLines = if ($primaryInternal.Count -gt 0) {
    @(
        $primaryInternal |
        Group-Object TargetFunctionRva |
        Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
        ForEach-Object {
            $first = $_.Group | Select-Object -First 1
            "Target=0x$('{0:X8}' -f [uint32]$_.Name) | Calls=$($_.Count) | TargetSection=$($first.TargetSection) | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
        }
    )
} else {
    @('No direct relative internal calls captured.')
}
Add-Section -Lines $lines -Title ("Primary Internal 0x{0:X8}" -f $primaryFunction.BeginRva) -Body $primaryInternalLines

$primaryIatLines = if ($primaryIat.Count -gt 0) {
    @(
        $primaryIat |
        Group-Object TargetLabel |
        Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
        ForEach-Object {
            $first = $_.Group | Select-Object -First 1
            "Import=$($_.Name) | Calls=$($_.Count) | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
        }
    )
} else {
    @('No direct RIP-relative IAT calls captured.')
}
Add-Section -Lines $lines -Title ("Primary IAT 0x{0:X8}" -f $primaryFunction.BeginRva) -Body $primaryIatLines

foreach ($followRva in $FollowTargetRvas) {
    $followFunction = Find-ContainingFunction -CodeRva $followRva -Functions $functions
    if (-not $followFunction) {
        Add-Section -Lines $lines -Title ("Follow Target 0x{0:X8}" -f $followRva) -Body @(
            'Could not resolve this RVA to a runtime function.'
        )
        continue
    }

    $followUnwind = Parse-UnwindInfoSummary -Bytes $bytes -Layout $layout -UnwindRva $followFunction.UnwindRva
    $followNeighbors = Get-NeighborFunctions -Function $followFunction -Functions $functions -Radius $NeighborRadius
    $followCalls = Find-DirectCallSitesForFunction -Bytes $bytes -Layout $layout -Function $followFunction -Functions $functions -Imports $imports
    $followInternal = @($followCalls | Where-Object { $_.InstructionKind -eq 'CALL_REL' })
    $followIat = @($followCalls | Where-Object { $_.InstructionKind -ne 'CALL_REL' })
    $followCandidate = Find-CandidateByRva -Rva $followRva -Candidates $clusterCandidates
    $bandHits = Get-FeatureBandHits -Rva $followRva -Bands $featureBands
    $nearestCandidates = Find-NearestCandidates -Rva $followRva -Candidates $clusterCandidates -Count 5
    $featureHitLines = if ($bandHits.Count -gt 0) {
        @($bandHits | ForEach-Object { "$($_.Feature) | FunctionBand=0x$('{0:X8}' -f $_.BeginRva) .. 0x$('{0:X8}' -f $_.EndRva)" })
    } else {
        @('<none>')
    }
    $candidateLine = if ($followCandidate) {
        @("ContainsKnownCandidate: Feature=$($followCandidate.Feature) | Function=0x$('{0:X8}' -f $followCandidate.BeginRva)-0x$('{0:X8}' -f $followCandidate.EndRva) | Matches=$($followCandidate.Matches)")
    } else {
        @('ContainsKnownCandidate: <none>')
    }
    $nearestLines = @(
        $nearestCandidates | ForEach-Object {
            $distance = [Math]::Min([Math]::Abs([int64]$followRva - [int64]$_.BeginRva), [Math]::Abs([int64]$followRva - [int64]$_.EndRva))
            "Feature=$($_.Feature) | Function=0x$('{0:X8}' -f $_.BeginRva)-0x$('{0:X8}' -f $_.EndRva) | Distance=$distance | Matches=$($_.Matches)"
        }
    )

    Add-Section -Lines $lines -Title ("Follow Target 0x{0:X8}" -f $followRva) -Body @(
        "Function           : $(Format-FunctionLabel -Function $followFunction)",
        "Section            : $($followFunction.Section)",
        "Size               : $($followFunction.Size)",
        "UnwindRva          : 0x$('{0:X8}' -f $followFunction.UnwindRva)",
        "UnwindInfo         : $followUnwind",
        "DirectInternal     : $($followInternal.Count)",
        "DirectIat          : $($followIat.Count)"
    )

    Add-Section -Lines $lines -Title ("Feature Band Hits 0x{0:X8}" -f $followRva) -Body $featureHitLines
    Add-Section -Lines $lines -Title ("Known Candidate Match 0x{0:X8}" -f $followRva) -Body $candidateLine
    Add-Section -Lines $lines -Title ("Nearest Candidates 0x{0:X8}" -f $followRva) -Body $nearestLines

    $followNeighborLines = @(
        $followNeighbors | ForEach-Object {
            $neighbor = $_.Function
            $neighborUnwind = Parse-UnwindInfoSummary -Bytes $bytes -Layout $layout -UnwindRva $neighbor.UnwindRva
            $marker = if ($_.RelativeIndex -eq 0) { '<follow>' } elseif ($_.RelativeIndex -lt 0) { ('prev{0}' -f ([Math]::Abs($_.RelativeIndex))) } else { ('next{0}' -f $_.RelativeIndex) }
            "Slot=$marker | Function=$(Format-FunctionLabel -Function $neighbor) | Section=$($neighbor.Section) | Size=$($neighbor.Size) | Unwind=0x$('{0:X8}' -f $neighbor.UnwindRva) | UnwindInfo=$neighborUnwind"
        }
    )
    Add-Section -Lines $lines -Title ("Neighbors 0x{0:X8}" -f $followFunction.BeginRva) -Body $followNeighborLines

    $followInternalLines = if ($followInternal.Count -gt 0) {
        @(
            $followInternal |
            Group-Object TargetFunctionRva |
            Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
            ForEach-Object {
                $first = $_.Group | Select-Object -First 1
                $candidate = Find-CandidateByRva -Rva $first.TargetFunctionRva -Candidates $clusterCandidates
                $featureLabel = if ($candidate) { $candidate.Feature } else { '<none>' }
                "Target=0x$('{0:X8}' -f [uint32]$_.Name) | Calls=$($_.Count) | TargetSection=$($first.TargetSection) | FeatureHit=$featureLabel | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
            }
        )
    } else {
        @('No direct relative internal calls captured.')
    }
    Add-Section -Lines $lines -Title ("Internal Calls 0x{0:X8}" -f $followFunction.BeginRva) -Body $followInternalLines

    $followIatLines = if ($followIat.Count -gt 0) {
        @(
            $followIat |
            Group-Object TargetLabel |
            Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
            ForEach-Object {
                $first = $_.Group | Select-Object -First 1
                "Import=$($_.Name) | Calls=$($_.Count) | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
            }
        )
    } else {
        @('No direct RIP-relative IAT calls captured.')
    }
    Add-Section -Lines $lines -Title ("IAT Calls 0x{0:X8}" -f $followFunction.BeginRva) -Body $followIatLines
}

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only and single-target. It centers on 0x00006E74 and its two direct follow-on targets from the previous branch-split assessment.',
    'Known machinery is judged two ways: exact containment within current cluster-profile candidates, and looser membership in current feature-map function bands.',
    'The output is intended to answer whether the 0x00006E74 branch drifts back into endpoint/transfer machinery or leaves that neighborhood.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "USBXHCI single-target deep recon written to $resolvedOutputPath"
