[CmdletBinding()]
param(
    [string]$ImagePath,
    [string]$FeatureMapPath,
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
    return (Join-Path $defaultDir 'usbxhci-cluster-profile.txt')
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
    $machine = Read-UInt16LE -Bytes $Bytes -Offset $coffOffset
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

        $name = [System.Text.Encoding]::ASCII.GetString($nameBytes, 0, $nullIndex)
        $virtualSize = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 8)
        $virtualAddress = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 12)
        $sizeOfRawData = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 16)
        $pointerToRawData = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 20)
        $sections += [pscustomobject]@{
            Name             = $name
            VirtualSize      = $virtualSize
            VirtualAddress   = $virtualAddress
            SizeOfRawData    = $sizeOfRawData
            PointerToRawData = $pointerToRawData
        }
    }

    return [pscustomobject]@{
        Machine        = $machine
        TimeDateStamp  = $timeDateStamp
        EntryPointRva  = $entryPointRva
        SizeOfHeaders  = $sizeOfHeaders
        DataDirectories = $dataDirectories
        Sections       = $sections
    }
}

function Get-DataDirectory {
    param(
        [object]$Layout,
        [int]$Index
    )

    return ($Layout.DataDirectories | Where-Object { $_.Index -eq $Index } | Select-Object -First 1)
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

        $functions += [pscustomobject]@{
            BeginRva  = $beginRva
            EndRva    = $endRva
            Size      = ($endRva - $beginRva)
            UnwindRva = $unwindRva
            Section   = Get-SectionNameForRva -Rva $beginRva -Sections $Layout.Sections
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

            $importName = Read-AsciiZ -Bytes $Bytes -Offset ($hintNameOffset + 2)
            $imports += [pscustomobject]@{
                DllName    = $dllName
                ImportName = $importName
                IatRva     = [uint32]($firstThunk + ($thunkIndex * $thunkSize))
            }
        }
    }

    return $imports
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

function Parse-FeatureMapCandidates {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Feature map not found: $Path"
    }

    $candidates = New-Object System.Collections.Generic.List[object]
    $currentFeature = $null
    foreach ($line in Get-Content $Path) {
        if ($line -match '^## (?<feature>.+) Candidate Functions$') {
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

        if ($line -match '^Function=0x(?<begin>[0-9A-Fa-f]+)-0x(?<end>[0-9A-Fa-f]+) \| Section=(?<section>[^|]+) \| RefCount=(?<count>\d+) \| Matches=(?<matches>[^|]+) \| Anchors=(?<anchors>.+)$') {
            $candidates.Add([pscustomobject]@{
                Feature         = $currentFeature
                BeginRva        = [uint32]('0x' + $matches['begin'])
                EndRva          = [uint32]('0x' + $matches['end'])
                Section         = $matches['section'].Trim()
                ReferenceCount  = [int]$matches['count']
                AnchorMatches   = $matches['matches'].Trim()
                AnchorTexts     = $matches['anchors'].Trim()
            }) | Out-Null
        }
    }

    return $candidates
}

function Find-IatCallSites {
    param(
        [byte[]]$Bytes,
        [object]$Layout,
        [object[]]$Imports,
        [object[]]$Functions
    )

    $importsByIatRva = @{}
    foreach ($import in $Imports) {
        $importsByIatRva[[uint32]$import.IatRva] = $import
    }

    $hits = New-Object System.Collections.Generic.List[object]
    foreach ($section in $Layout.Sections) {
        $sectionEndRva = $section.VirtualAddress + [Math]::Max([uint32]$section.VirtualSize, [uint32]$section.SizeOfRawData)
        $isExecutable = ($section.Name -eq '.text' -or $section.Name -eq 'PAGE' -or $section.Name -eq 'PAGEIPPT' -or $section.Name -eq 'INIT' -or $section.Name -eq 'fothk')
        if (-not $isExecutable) {
            continue
        }

        $start = [int]$section.PointerToRawData
        $end = [Math]::Min([int]($section.PointerToRawData + $section.SizeOfRawData), $Bytes.Length)
        for ($offset = $start; $offset -le ($end - 6); $offset++) {
            $opcode = $Bytes[$offset]
            $modrm = $Bytes[$offset + 1]
            if (($opcode -ne 0xFF) -or ($modrm -ne 0x15 -and $modrm -ne 0x25)) {
                continue
            }

            $instructionRva = [uint32]($section.VirtualAddress + ($offset - $section.PointerToRawData))
            if ($instructionRva -ge $sectionEndRva) {
                continue
            }

            $disp = Read-Int32LE -Bytes $Bytes -Offset ($offset + 2)
            $nextRva = [uint32]($instructionRva + 6)
            $targetRva64 = [int64]$nextRva + [int64]$disp
            if ($targetRva64 -lt 0 -or $targetRva64 -gt [uint32]::MaxValue) {
                continue
            }
            $targetRva = [uint32]$targetRva64
            $import = $importsByIatRva[$targetRva]
            if (-not $import) {
                continue
            }

            $function = Find-ContainingFunction -CodeRva $instructionRva -Functions $Functions
            $functionBeginRva = if ($function) { $function.BeginRva } else { $null }
            $functionEndRva = if ($function) { $function.EndRva } else { $null }
            $functionSection = if ($function) { $function.Section } else { $section.Name }
            $unwindRva = if ($function) { $function.UnwindRva } else { [uint32]0 }

            $hits.Add([pscustomobject]@{
                InstructionKind  = if ($modrm -eq 0x15) { 'CALL_IAT' } else { 'JMP_IAT' }
                InstructionRva   = $instructionRva
                InstructionHex   = Format-HexBytes -Bytes $Bytes -Offset $offset -Count 6
                CodeSection      = $section.Name
                IatRva           = $targetRva
                DllName          = $import.DllName
                ImportName       = $import.ImportName
                FunctionBeginRva = $functionBeginRva
                FunctionEndRva   = $functionEndRva
                FunctionSection  = $functionSection
                FunctionSize     = if ($function) { $function.Size } else { $null }
                UnwindRva        = $unwindRva
            }) | Out-Null
        }
    }

    return ($hits | Sort-Object FunctionBeginRva, InstructionRva)
}

function Get-CandidateProfiles {
    param(
        [object[]]$Candidates,
        [object[]]$IatHits,
        [byte[]]$Bytes,
        [object]$Layout
    )

    $profiles = New-Object System.Collections.Generic.List[object]
    foreach ($candidate in $Candidates) {
        $hits = @(
            $IatHits | Where-Object {
                $null -ne $_.FunctionBeginRva -and
                $_.FunctionBeginRva -eq $candidate.BeginRva -and
                $_.FunctionEndRva -eq $candidate.EndRva
            }
        )

        $imports = @(
            $hits |
            ForEach-Object { '{0}!{1}' -f $_.DllName, $_.ImportName } |
            Sort-Object -Unique
        )

        $unwindRva = if ($hits.Count -gt 0) { $hits[0].UnwindRva } else { [uint32]0 }
        $unwindSummary = Parse-UnwindInfoSummary -Bytes $Bytes -Layout $Layout -UnwindRva $unwindRva

        $profiles.Add([pscustomobject]@{
            Feature        = $candidate.Feature
            BeginRva       = $candidate.BeginRva
            EndRva         = $candidate.EndRva
            Section        = $candidate.Section
            ReferenceCount = $candidate.ReferenceCount
            AnchorMatches  = $candidate.AnchorMatches
            AnchorTexts    = $candidate.AnchorTexts
            IatHitCount    = $hits.Count
            UniqueImports  = $imports
            UnwindRva      = $unwindRva
            UnwindSummary  = $unwindSummary
        }) | Out-Null
    }

    return $profiles
}

$resolvedImagePath = Resolve-ImagePath -RequestedPath $ImagePath
if (-not (Test-Path $resolvedImagePath)) {
    throw "USBXHCI image not found: $resolvedImagePath"
}

$resolvedFeatureMapPath = Resolve-FeatureMapPath -RequestedPath $FeatureMapPath
$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath
$outputDir = Split-Path -Parent $resolvedOutputPath
if ($outputDir) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

$bytes = [System.IO.File]::ReadAllBytes($resolvedImagePath)
$file = Get-Item $resolvedImagePath
$layout = Parse-PeLayout -Bytes $bytes
$functions = Parse-RuntimeFunctions -Bytes $bytes -Layout $layout
$imports = Parse-ImportsWithIat -Bytes $bytes -Layout $layout
$candidates = Parse-FeatureMapCandidates -Path $resolvedFeatureMapPath
$iatHits = Find-IatCallSites -Bytes $bytes -Layout $layout -Imports $imports -Functions $functions
$profiles = Get-CandidateProfiles -Candidates $candidates -IatHits $iatHits -Bytes $bytes -Layout $layout

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Cluster Profile Recon') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "ImagePath           : $resolvedImagePath",
    "FileVersion         : $($file.VersionInfo.FileVersion)",
    "ProductVersion      : $($file.VersionInfo.ProductVersion)",
    "TimeDateStamp       : 0x$('{0:X8}' -f $layout.TimeDateStamp) ($(Convert-TimeDateStamp -TimeDateStamp $layout.TimeDateStamp))",
    "FeatureMapPath      : $resolvedFeatureMapPath",
    "RuntimeFunctions    : $($functions.Count)",
    "ImportIatEntries    : $($imports.Count)",
    "IatCallOrJumpHits   : $($iatHits.Count)",
    "CandidateFunctions  : $($candidates.Count)"
)

foreach ($featureGroup in ($profiles | Group-Object Feature | Sort-Object Name)) {
    $featureProfiles = @(
        $featureGroup.Group |
        Sort-Object -Property @{ Expression = 'IatHitCount'; Descending = $true }, 'BeginRva'
    )
    $importHistogram = @(
        $featureProfiles |
        ForEach-Object { $_.UniqueImports } |
        Group-Object |
        Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, 'Name' |
        Select-Object -First 12 |
        ForEach-Object { '{0} ({1})' -f $_.Name, $_.Count }
    )

    $pageableCount = @($featureProfiles | Where-Object { $_.Section -eq 'PAGE' -or $_.Section -eq 'PAGEIPPT' }).Count
    $nonPageableCount = $featureProfiles.Count - $pageableCount
    Add-Section -Lines $lines -Title "$($featureGroup.Name) Summary" -Body @(
        "Candidates       : $($featureProfiles.Count)",
        "Pageable         : $pageableCount",
        "NonPageable      : $nonPageableCount",
        "TopImports       : $(if ($importHistogram.Count -gt 0) { [string]::Join('; ', $importHistogram) } else { '<none>' })"
    )

    Add-Section -Lines $lines -Title "$($featureGroup.Name) Candidate Profiles" -Body @(
        ($featureProfiles | Select-Object -First 20 | ForEach-Object {
            $importList = if ($_.UniqueImports.Count -gt 0) { [string]::Join(', ', $_.UniqueImports) } else { '<none>' }
            "Function=0x$('{0:X8}' -f $_.BeginRva)-0x$('{0:X8}' -f $_.EndRva) | Section=$($_.Section) | RefCount=$($_.ReferenceCount) | IatHits=$($_.IatHitCount) | Unwind=0x$('{0:X8}' -f $_.UnwindRva) | UnwindInfo=$($_.UnwindSummary) | Imports=$importList | Matches=$($_.AnchorMatches)"
        })
    )
}

Add-Section -Lines $lines -Title 'Global Import Hotspots' -Body @(
    ($iatHits |
        ForEach-Object { '{0}!{1}' -f $_.DllName, $_.ImportName } |
        Group-Object |
        Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, 'Name' |
        Select-Object -First 25 |
        ForEach-Object { '{0} ({1})' -f $_.Name, $_.Count })
)

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is still heuristic and read-only. It identifies direct RIP-relative IAT call/jump sites only.',
    'Indirect call patterns that materialize imported pointers through intermediate loads are not counted here.',
    'The output is intended to separate likely control-plane, pageable code from nonpageable transfer/interrupter paths before any host-stack experiment.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "USBXHCI cluster profile written to $resolvedOutputPath"
