[CmdletBinding()]
param(
    [string]$ImagePath,
    [string]$ClusterProfilePath,
    [string[]]$Features = @('Transfer', 'Ring'),
    [int]$TopPerFeature = 4,
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

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    $defaultDir = Join-Path (Get-RepoRoot) 'out\dev'
    New-Item -ItemType Directory -Force -Path $defaultDir | Out-Null
    return (Join-Path $defaultDir 'usbxhci-targeted-callmap.txt')
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

function Parse-ClusterProfileCandidates {
    param(
        [string]$Path,
        [string[]]$FeatureNames,
        [int]$TopPerFeature
    )

    if (-not (Test-Path $Path)) {
        throw "Cluster profile not found: $Path"
    }

    $featureSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($feature in $FeatureNames) {
        [void]$featureSet.Add($feature)
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

        if (-not $currentFeature -or -not $featureSet.Contains($currentFeature)) {
            continue
        }

        if ($line -match '^Function=0x(?<begin>[0-9A-Fa-f]+)-0x(?<end>[0-9A-Fa-f]+) \| Section=(?<section>[^|]+) \| RefCount=(?<ref>\d+) \| IatHits=(?<iat>\d+) \| Unwind=0x(?<unwind>[0-9A-Fa-f]+) \| UnwindInfo=(?<unwindInfo>[^|]+) \| Imports=(?<imports>[^|]+) \| Matches=(?<matches>.+)$') {
            $imports = @()
            if ($matches['imports'].Trim() -ne '<none>') {
                $imports = @($matches['imports'].Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
            }

            $candidates.Add([pscustomobject]@{
                Feature        = $currentFeature
                BeginRva       = [uint32]('0x' + $matches['begin'])
                EndRva         = [uint32]('0x' + $matches['end'])
                Section        = $matches['section'].Trim()
                ReferenceCount = [int]$matches['ref']
                IatHits        = [int]$matches['iat']
                UnwindRva      = [uint32]('0x' + $matches['unwind'])
                UnwindInfo     = $matches['unwindInfo'].Trim()
                Imports        = $imports
                Matches        = $matches['matches'].Trim()
            }) | Out-Null
        }
    }

    $selected = New-Object System.Collections.Generic.List[object]
    foreach ($feature in $FeatureNames) {
        $top = @(
            $candidates |
            Where-Object { $_.Feature -eq $feature } |
            Sort-Object -Property @{ Expression = 'IatHits'; Descending = $true }, @{ Expression = 'ReferenceCount'; Descending = $true }, 'BeginRva' |
            Select-Object -First $TopPerFeature
        )
        foreach ($item in $top) {
            $selected.Add($item) | Out-Null
        }
    }

    return $selected
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

function Build-TargetIndex {
    param([object[]]$Targets)

    $byBeginRva = @{}
    foreach ($target in $Targets) {
        $byBeginRva[[uint32]$target.BeginRva] = $target
    }

    return $byBeginRva
}

function Find-TargetByRva {
    param(
        [uint32]$Rva,
        [object[]]$Targets
    )

    foreach ($target in $Targets) {
        if ($Rva -ge $target.BeginRva -and $Rva -lt $target.EndRva) {
            return $target
        }
    }

    return $null
}

function Find-TargetedCallSites {
    param(
        [byte[]]$Bytes,
        [object]$Layout,
        [object[]]$Targets,
        [object[]]$Functions,
        [object[]]$Imports
    )

    $importsByIatRva = @{}
    foreach ($import in $Imports) {
        $importsByIatRva[[uint32]$import.IatRva] = $import
    }

    $sites = New-Object System.Collections.Generic.List[object]
    foreach ($target in $Targets) {
        $offset = Convert-RvaToOffset -Rva $target.BeginRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
        if ($null -eq $offset) {
            continue
        }

        $length = [int]($target.EndRva - $target.BeginRva)
        $endOffset = [Math]::Min($offset + $length, $Bytes.Length)
        for ($cursor = $offset; $cursor -lt $endOffset; $cursor++) {
            $instructionRva = [uint32]($target.BeginRva + ($cursor - $offset))

            if (($cursor + 5) -le $endOffset -and $Bytes[$cursor] -eq 0xE8) {
                $disp = Read-Int32LE -Bytes $Bytes -Offset ($cursor + 1)
                $nextRva = [uint32]($instructionRva + 5)
                $targetRva64 = [int64]$nextRva + [int64]$disp
                if ($targetRva64 -ge 0 -and $targetRva64 -le [uint32]::MaxValue) {
                    $calleeRva = [uint32]$targetRva64
                    $calleeFunction = Find-ContainingFunction -CodeRva $calleeRva -Functions $Functions
                    if (-not $calleeFunction) {
                        continue
                    }
                    $calleeTarget = Find-TargetByRva -Rva $calleeRva -Targets $Targets
                    $sites.Add([pscustomobject]@{
                        SourceFeature      = $target.Feature
                        SourceBeginRva     = $target.BeginRva
                        SourceEndRva       = $target.EndRva
                        SourceSection      = $target.Section
                        InstructionKind    = 'CALL_REL'
                        InstructionRva     = $instructionRva
                        InstructionHex     = Format-HexBytes -Bytes $Bytes -Offset $cursor -Count 5
                        TargetRva          = $calleeRva
                        TargetFunctionRva  = if ($calleeFunction) { $calleeFunction.BeginRva } else { $null }
                        TargetFunctionEnd  = if ($calleeFunction) { $calleeFunction.EndRva } else { $null }
                        TargetSection      = $calleeFunction.Section
                        TargetFeature      = if ($calleeTarget) { $calleeTarget.Feature } else { $null }
                        TargetLabel        = if ($calleeTarget) { ('{0}:0x{1:X8}' -f $calleeTarget.Feature, $calleeTarget.BeginRva) } else { ('0x{0:X8}' -f $calleeFunction.BeginRva) }
                        ImportName         = $null
                    }) | Out-Null
                }
                continue
            }

            if (($cursor + 6) -le $endOffset -and $Bytes[$cursor] -eq 0xFF -and ($Bytes[$cursor + 1] -eq 0x15 -or $Bytes[$cursor + 1] -eq 0x25)) {
                $disp = Read-Int32LE -Bytes $Bytes -Offset ($cursor + 2)
                $nextRva = [uint32]($instructionRva + 6)
                $iatRva64 = [int64]$nextRva + [int64]$disp
                if ($iatRva64 -lt 0 -or $iatRva64 -gt [uint32]::MaxValue) {
                    continue
                }
                $iatRva = [uint32]$iatRva64
                $import = $importsByIatRva[$iatRva]
                if (-not $import) {
                    continue
                }

                $sites.Add([pscustomobject]@{
                    SourceFeature      = $target.Feature
                    SourceBeginRva     = $target.BeginRva
                    SourceEndRva       = $target.EndRva
                    SourceSection      = $target.Section
                    InstructionKind    = if ($Bytes[$cursor + 1] -eq 0x15) { 'CALL_IAT' } else { 'JMP_IAT' }
                    InstructionRva     = $instructionRva
                    InstructionHex     = Format-HexBytes -Bytes $Bytes -Offset $cursor -Count 6
                    TargetRva          = $iatRva
                    TargetFunctionRva  = $null
                    TargetFunctionEnd  = $null
                    TargetSection      = '.idata'
                    TargetFeature      = $null
                    TargetLabel        = ('{0}!{1}' -f $import.DllName, $import.ImportName)
                    ImportName         = ('{0}!{1}' -f $import.DllName, $import.ImportName)
                }) | Out-Null
            }
        }
    }

    return ($sites | Sort-Object SourceFeature, SourceBeginRva, InstructionRva)
}

$resolvedImagePath = Resolve-ImagePath -RequestedPath $ImagePath
if (-not (Test-Path $resolvedImagePath)) {
    throw "USBXHCI image not found: $resolvedImagePath"
}

$resolvedClusterProfilePath = Resolve-ClusterProfilePath -RequestedPath $ClusterProfilePath
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
$targets = Parse-ClusterProfileCandidates -Path $resolvedClusterProfilePath -FeatureNames $Features -TopPerFeature $TopPerFeature
$callSites = Find-TargetedCallSites -Bytes $bytes -Layout $layout -Targets $targets -Functions $functions -Imports $imports

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Targeted Call Map') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "ImagePath           : $resolvedImagePath",
    "FileVersion         : $($file.VersionInfo.FileVersion)",
    "ProductVersion      : $($file.VersionInfo.ProductVersion)",
    "TimeDateStamp       : 0x$('{0:X8}' -f $layout.TimeDateStamp) ($(Convert-TimeDateStamp -TimeDateStamp $layout.TimeDateStamp))",
    "ClusterProfilePath  : $resolvedClusterProfilePath",
    "SelectedFeatures    : $([string]::Join(', ', $Features))",
    "TopPerFeature       : $TopPerFeature",
    "SelectedFunctions   : $($targets.Count)",
    "CallSitesCaptured   : $($callSites.Count)"
)

foreach ($feature in $Features) {
    $featureTargets = @($targets | Where-Object { $_.Feature -eq $feature } | Sort-Object BeginRva)
    if ($featureTargets.Count -eq 0) {
        continue
    }

    $featureTargetLines = @(
        $featureTargets | ForEach-Object {
            $importList = if ($_.Imports.Count -gt 0) { [string]::Join(', ', $_.Imports) } else { '<none>' }
            "Function=0x$('{0:X8}' -f $_.BeginRva)-0x$('{0:X8}' -f $_.EndRva) | Section=$($_.Section) | IatHits=$($_.IatHits) | Imports=$importList | Matches=$($_.Matches)"
        }
    )

    Add-Section -Lines $lines -Title "$feature Target Functions" -Body @(
        $featureTargetLines
    )

    foreach ($target in $featureTargets) {
        $targetSites = @(
            $callSites |
            Where-Object {
                $_.SourceBeginRva -eq $target.BeginRva -and
                $_.SourceEndRva -eq $target.EndRva
            }
        )

        $internalSites = @($targetSites | Where-Object { $_.InstructionKind -eq 'CALL_REL' })
        $iatSites = @($targetSites | Where-Object { $_.InstructionKind -ne 'CALL_REL' })
        $crossFeature = @($internalSites | Where-Object { $_.TargetFeature -and $_.TargetFeature -ne $target.Feature })
        $sameFeature = @($internalSites | Where-Object { $_.TargetFeature -eq $target.Feature })

        Add-Section -Lines $lines -Title ("Call Map {0} 0x{1:X8}-0x{2:X8}" -f $target.Feature, $target.BeginRva, $target.EndRva) -Body @(
            "Section            : $($target.Section)",
            "Matches            : $($target.Matches)",
            "IatHitsProfile     : $($target.IatHits)",
            "DirectInternalCalls: $($internalSites.Count)",
            "DirectIatCalls     : $($iatSites.Count)",
            "SameFeatureCalls   : $($sameFeature.Count)",
            "CrossFeatureCalls  : $($crossFeature.Count)"
        )

        $internalCallLines = if ($internalSites.Count -gt 0) {
                $internalSites |
                Group-Object TargetLabel |
                Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
                Select-Object -First 20 |
                ForEach-Object {
                    $first = $_.Group | Select-Object -First 1
                    $targetFeatureLabel = if ($first.TargetFeature) { $first.TargetFeature } else { '<none>' }
                    "Target=$($_.Name) | Calls=$($_.Count) | TargetSection=$($first.TargetSection) | TargetFeature=$targetFeatureLabel | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
                }
            } else {
                @('No direct relative internal calls captured.')
            }

        Add-Section -Lines $lines -Title ("Internal Calls {0} 0x{1:X8}" -f $target.Feature, $target.BeginRva) -Body @(
            $internalCallLines
        )

        $iatCallLines = if ($iatSites.Count -gt 0) {
                $iatSites |
                Group-Object TargetLabel |
                Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
                Select-Object -First 20 |
                ForEach-Object {
                    $first = $_.Group | Select-Object -First 1
                    "Import=$($_.Name) | Calls=$($_.Count) | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
                }
            } else {
                @('No direct RIP-relative IAT calls captured.')
            }

        Add-Section -Lines $lines -Title ("IAT Calls {0} 0x{1:X8}" -f $target.Feature, $target.BeginRva) -Body @(
            $iatCallLines
        )
    }

    $featureEdges = @($callSites | Where-Object { $_.SourceFeature -eq $feature -and $_.TargetFeature })
    $featureEdgeLines = if ($featureEdges.Count -gt 0) {
            $featureEdges |
            Group-Object { '{0}->{1}' -f $_.SourceFeature, $_.TargetFeature } |
            Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
            ForEach-Object { "$($_.Name) | Calls=$($_.Count)" }
        } else {
            @('No cross-feature internal edges captured.')
        }

    Add-Section -Lines $lines -Title "$feature Cross-Feature Edges" -Body @(
        $featureEdgeLines
    )
}

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only and intentionally narrow. It only maps the top selected transfer and ring clusters from the current cluster profile.',
    'Internal edges are direct E8 relative calls only. Imported edges are direct RIP-relative CALL/JMP IAT sites only.',
    'The output is intended to support offline reverse-engineering prioritization, not to prove full semantic call graphs.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "USBXHCI targeted call map written to $resolvedOutputPath"
