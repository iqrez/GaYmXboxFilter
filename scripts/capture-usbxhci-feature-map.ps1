[CmdletBinding()]
param(
    [string]$ImagePath,
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

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    $defaultDir = Join-Path (Get-RepoRoot) 'out\dev'
    New-Item -ItemType Directory -Force -Path $defaultDir | Out-Null
    return (Join-Path $defaultDir 'usbxhci-feature-map.txt')
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

function Get-ByteSlice {
    param(
        [byte[]]$Bytes,
        [int]$Offset,
        [int]$Length
    )

    if ($Length -le 0 -or $Offset -lt 0 -or ($Offset + $Length) -gt $Bytes.Length) {
        return @()
    }

    $slice = New-Object byte[] $Length
    [Buffer]::BlockCopy($Bytes, $Offset, $slice, 0, $Length)
    return $slice
}

function Format-HexBytes {
    param(
        [byte[]]$Bytes,
        [int]$Offset,
        [int]$Count
    )

    $actualCount = [Math]::Min($Count, $Bytes.Length - $Offset)
    if ($actualCount -le 0) {
        return ''
    }

    $slice = Get-ByteSlice -Bytes $Bytes -Offset $Offset -Length $actualCount
    return (($slice | ForEach-Object { $_.ToString('X2') }) -join ' ')
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

function Convert-TimeDateStamp {
    param([uint32]$TimeDateStamp)
    return ([DateTimeOffset]::FromUnixTimeSeconds([int64]$TimeDateStamp)).ToString('yyyy-MM-dd HH:mm:ss zzz')
}

function Get-MachineName {
    param([uint16]$Machine)

    switch ($Machine) {
        0x014c { return 'I386' }
        0x8664 { return 'AMD64' }
        0x01c0 { return 'ARM' }
        0xAA64 { return 'ARM64' }
        default { return ('0x{0:X4}' -f $Machine) }
    }
}

function Format-SectionFlags {
    param([uint32]$Characteristics)

    $flags = New-Object System.Collections.Generic.List[string]
    if ($Characteristics -band 0x00000020) { $flags.Add('CODE') | Out-Null }
    if ($Characteristics -band 0x00000040) { $flags.Add('INIT_DATA') | Out-Null }
    if ($Characteristics -band 0x00000080) { $flags.Add('UNINIT_DATA') | Out-Null }
    if ($Characteristics -band 0x20000000) { $flags.Add('EXECUTE') | Out-Null }
    if ($Characteristics -band 0x40000000) { $flags.Add('READ') | Out-Null }
    if ($Characteristics -band 0x80000000) { $flags.Add('WRITE') | Out-Null }
    return ($flags -join ', ')
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
    $imageBase = if ($isPe32Plus) {
        Read-UInt64LE -Bytes $Bytes -Offset ($optionalHeaderOffset + 24)
    } else {
        [uint64](Read-UInt32LE -Bytes $Bytes -Offset ($optionalHeaderOffset + 28))
    }
    $sizeOfImage = Read-UInt32LE -Bytes $Bytes -Offset ($optionalHeaderOffset + 56)
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
        $nameBytes = Get-ByteSlice -Bytes $Bytes -Offset $sectionOffset -Length 8
        $nullIndex = [Array]::IndexOf($nameBytes, [byte]0)
        if ($nullIndex -lt 0) {
            $nullIndex = $nameBytes.Length
        }

        $name = [System.Text.Encoding]::ASCII.GetString($nameBytes, 0, $nullIndex)
        $virtualSize = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 8)
        $virtualAddress = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 12)
        $sizeOfRawData = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 16)
        $pointerToRawData = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 20)
        $characteristics = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 36)

        $sections += [pscustomobject]@{
            Name             = $name
            VirtualSize      = $virtualSize
            VirtualAddress   = $virtualAddress
            SizeOfRawData    = $sizeOfRawData
            PointerToRawData = $pointerToRawData
            Characteristics  = $characteristics
            Flags            = Format-SectionFlags -Characteristics $characteristics
        }
    }

    return [pscustomobject]@{
        Machine        = $machine
        TimeDateStamp  = $timeDateStamp
        SectionCount   = $sectionCount
        IsPe32Plus     = $isPe32Plus
        EntryPointRva  = $entryPointRva
        ImageBase      = $imageBase
        SizeOfImage    = $sizeOfImage
        SizeOfHeaders  = $sizeOfHeaders
        Sections       = $sections
        DataDirectories = $dataDirectories
    }
}

function Get-DataDirectory {
    param(
        [object]$Layout,
        [int]$Index
    )

    return ($Layout.DataDirectories | Where-Object { $_.Index -eq $Index } | Select-Object -First 1)
}

function Get-SectionForRva {
    param(
        [uint32]$Rva,
        [object[]]$Sections
    )

    foreach ($section in $Sections) {
        $sectionSpan = [Math]::Max([uint32]$section.VirtualSize, [uint32]$section.SizeOfRawData)
        if ($Rva -ge $section.VirtualAddress -and $Rva -lt ($section.VirtualAddress + $sectionSpan)) {
            return $section
        }
    }

    return $null
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
            Section   = (Get-SectionForRva -Rva $beginRva -Sections $Layout.Sections).Name
        }
    }

    return ($functions | Sort-Object BeginRva)
}

function Find-AsciiStrings {
    param(
        [byte[]]$Bytes,
        [object[]]$Sections,
        [uint32]$SizeOfHeaders,
        [int]$MinLength = 6
    )

    $strings = New-Object System.Collections.Generic.List[object]
    foreach ($section in $Sections | Where-Object { $_.Flags -like '*READ*' -and $_.Flags -notlike '*EXECUTE*' }) {
        $sectionStart = [int]$section.PointerToRawData
        $sectionEnd = [Math]::Min([int]($section.PointerToRawData + $section.SizeOfRawData), $Bytes.Length)
        $start = -1

        for ($offset = $sectionStart; $offset -lt $sectionEnd; $offset++) {
            $byte = $Bytes[$offset]
            $isPrintable = ($byte -ge 0x20 -and $byte -le 0x7E)
            if ($isPrintable) {
                if ($start -lt 0) {
                    $start = $offset
                }
                continue
            }

            if ($start -ge 0) {
                $length = $offset - $start
                if ($length -ge $MinLength) {
                    $rva = [uint32]($section.VirtualAddress + ($start - $section.PointerToRawData))
                    $strings.Add([pscustomobject]@{
                        FileOffset = $start
                        RVA        = $rva
                        Section    = $section.Name
                        Text       = [System.Text.Encoding]::ASCII.GetString($Bytes, $start, $length)
                    }) | Out-Null
                }
                $start = -1
            }
        }
    }

    return $strings
}

function Get-FeatureDefinitions {
    return @(
        [pscustomobject]@{
            Name = 'Controller'
            Patterns = @(
                'controller.c',
                'Controller enumeration failure',
                'reset recovery',
                'controller stop timed out',
                'controller reset timed out',
                'controller start timed out',
                'WDFDRIVER_USBXHCI_CONTEXT',
                'deviceslot.c',
                'xildeviceslot.c',
                'usbdevice.c',
                'roothub.c',
                'commonbuffer.c'
            )
        },
        [pscustomobject]@{
            Name = 'Endpoint'
            Patterns = @('endpoint.c', 'ENDPOINT_DATA', 'Reset Endpoint', 'Stop Endpoint', 'Dequeue Pointer', 'Set Dequeue', 'Endpoint ')
        },
        [pscustomobject]@{
            Name = 'Ring'
            Patterns = @('command ring', 'CommandAbortRing', 'CommandQueryIsRingRunning', 'ring still running', 'ring is stopped', 'tr.c', 'TRB', 'Trb')
        },
        [pscustomobject]@{
            Name = 'Transfer'
            Patterns = @('Transfer Event', 'Stopped Transfer', 'Transfer Ring Tag', 'Transfer ')
        },
        [pscustomobject]@{
            Name = 'Interrupter'
            Patterns = @('INTERRUPTER_DATA', 'PRIMARY_INTERRUPTER_DATA', 'interrupts for internal XHCI', 'Interrupter', 'interrupt')
        }
    )
}

function Get-FeatureAnchors {
    param(
        [object[]]$Strings,
        [object[]]$FeatureDefinitions
    )

    $anchors = New-Object System.Collections.Generic.List[object]
    foreach ($feature in $FeatureDefinitions) {
        foreach ($candidate in $Strings) {
            foreach ($pattern in $feature.Patterns) {
                if ($candidate.Text.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    $anchors.Add([pscustomobject]@{
                        Feature    = $feature.Name
                        Match      = $pattern
                        Section    = $candidate.Section
                        RVA        = $candidate.RVA
                        FileOffset = $candidate.FileOffset
                        Text       = $candidate.Text
                    }) | Out-Null
                    break
                }
            }
        }
    }

    return ($anchors | Sort-Object Feature, RVA -Unique)
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

function Find-RipRelativeLeaReferences {
    param(
        [byte[]]$Bytes,
        [object]$Layout,
        [object[]]$Functions,
        [object[]]$Anchors
    )

    $anchorByRva = @{}
    foreach ($anchor in $Anchors) {
        $anchorByRva[[uint32]$anchor.RVA] = $anchor
    }

    $references = New-Object System.Collections.Generic.List[object]
    $r11OpcodeValues = @(0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D)
    $executableSections = $Layout.Sections | Where-Object { $_.Flags -like '*EXECUTE*' }

    foreach ($section in $executableSections) {
        $sectionStart = [int]$section.PointerToRawData
        $sectionEnd = [Math]::Min([int]($section.PointerToRawData + $section.SizeOfRawData), $Bytes.Length)
        for ($offset = $sectionStart; $offset -le ($sectionEnd - 7); $offset++) {
            $first = $Bytes[$offset]
            $second = $Bytes[$offset + 1]
            $third = $Bytes[$offset + 2]
            if (($first -ne 0x48 -and $first -ne 0x4C) -or $second -ne 0x8D -or ($third -notin $r11OpcodeValues)) {
                continue
            }

            $disp = Read-Int32LE -Bytes $Bytes -Offset ($offset + 3)
            $instructionRva = [uint32]($section.VirtualAddress + ($offset - $section.PointerToRawData))
            $nextRva = [uint32]($instructionRva + 7)
            $targetRva = [uint32]([int64]$nextRva + [int64]$disp)
            $anchor = $anchorByRva[$targetRva]
            if (-not $anchor) {
                continue
            }

            $function = Find-ContainingFunction -CodeRva $instructionRva -Functions $Functions
            $functionBeginRva = if ($function) { $function.BeginRva } else { $null }
            $functionEndRva = if ($function) { $function.EndRva } else { $null }
            $functionSection = if ($function) { $function.Section } else { $section.Name }

            $references.Add([pscustomobject]@{
                Feature         = $anchor.Feature
                AnchorText      = $anchor.Text
                AnchorMatch     = $anchor.Match
                AnchorRva       = $anchor.RVA
                CodeSection     = $section.Name
                InstructionRva  = $instructionRva
                InstructionHex  = Format-HexBytes -Bytes $Bytes -Offset $offset -Count 7
                FunctionBeginRva = $functionBeginRva
                FunctionEndRva   = $functionEndRva
                FunctionSection  = $functionSection
            }) | Out-Null
        }
    }

    return ($references | Sort-Object Feature, FunctionBeginRva, InstructionRva)
}

function Get-FeatureSummaries {
    param(
        [object[]]$Anchors,
        [object[]]$References,
        [object[]]$FeatureDefinitions
    )

    $summaries = @()
    foreach ($feature in $FeatureDefinitions) {
        $featureAnchors = @($Anchors | Where-Object { $_.Feature -eq $feature.Name })
        $featureReferences = @($References | Where-Object { $_.Feature -eq $feature.Name })
        $functionStarts = @($featureReferences | Where-Object { $null -ne $_.FunctionBeginRva } | Select-Object -ExpandProperty FunctionBeginRva -Unique | Sort-Object)
        $anchorBands = @(
            $featureAnchors |
            Sort-Object RVA |
            ForEach-Object { ('0x{0:X8}' -f $_.RVA) }
        )

        $anchorBand = if ($anchorBands.Count -gt 0) { "$($anchorBands[0]) .. $($anchorBands[-1])" } else { '<none>' }
        $functionBand = if ($functionStarts.Count -gt 0) { ('0x{0:X8}' -f $functionStarts[0]) + ' .. ' + ('0x{0:X8}' -f $functionStarts[-1]) } else { '<none>' }

        $summaries += [pscustomobject]@{
            Feature          = $feature.Name
            AnchorCount      = $featureAnchors.Count
            ReferenceCount   = $featureReferences.Count
            FunctionCount    = $functionStarts.Count
            AnchorBand       = $anchorBand
            FunctionBand     = $functionBand
        }
    }

    return $summaries
}

$resolvedImagePath = Resolve-ImagePath -RequestedPath $ImagePath
if (-not (Test-Path $resolvedImagePath)) {
    throw "USBXHCI image not found: $resolvedImagePath"
}

$resolvedOutputPath = Resolve-OutputPath -RequestedPath $OutputPath
$outputDir = Split-Path -Parent $resolvedOutputPath
if ($outputDir) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

$bytes = [System.IO.File]::ReadAllBytes($resolvedImagePath)
$file = Get-Item $resolvedImagePath
$layout = Parse-PeLayout -Bytes $bytes
$functions = Parse-RuntimeFunctions -Bytes $bytes -Layout $layout
$strings = Find-AsciiStrings -Bytes $bytes -Sections $layout.Sections -SizeOfHeaders $layout.SizeOfHeaders
$featureDefinitions = Get-FeatureDefinitions
$anchors = Get-FeatureAnchors -Strings $strings -FeatureDefinitions $featureDefinitions
$references = Find-RipRelativeLeaReferences -Bytes $bytes -Layout $layout -Functions $functions -Anchors $anchors
$featureSummaries = Get-FeatureSummaries -Anchors $anchors -References $references -FeatureDefinitions $featureDefinitions
$entryPointOffset = Convert-RvaToOffset -Rva $layout.EntryPointRva -Sections $layout.Sections -SizeOfHeaders $layout.SizeOfHeaders
$entryPointBytes = if ($null -ne $entryPointOffset) { Format-HexBytes -Bytes $bytes -Offset $entryPointOffset -Count 16 } else { '<unmapped>' }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Feature Map Recon') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "ImagePath        : $resolvedImagePath",
    "FileVersion      : $($file.VersionInfo.FileVersion)",
    "ProductVersion   : $($file.VersionInfo.ProductVersion)",
    "Machine          : $(Get-MachineName -Machine $layout.Machine)",
    "TimeDateStamp    : 0x$('{0:X8}' -f $layout.TimeDateStamp) ($(Convert-TimeDateStamp -TimeDateStamp $layout.TimeDateStamp))",
    "EntryPointRva    : 0x$('{0:X8}' -f $layout.EntryPointRva)",
    "EntryPointBytes  : $entryPointBytes",
    "RuntimeFunctions : $($functions.Count)",
    "FeatureAnchors   : $($anchors.Count)",
    "CodeReferences   : $($references.Count)"
)

Add-Section -Lines $lines -Title 'Feature Summary' -Body @(
    ($featureSummaries | ForEach-Object {
        "$($_.Feature) | Anchors=$($_.AnchorCount) | Refs=$($_.ReferenceCount) | Functions=$($_.FunctionCount) | AnchorBand=$($_.AnchorBand) | FunctionBand=$($_.FunctionBand)"
    })
)

foreach ($feature in $featureDefinitions) {
    $featureAnchors = @($anchors | Where-Object { $_.Feature -eq $feature.Name })
    $featureReferences = @($references | Where-Object { $_.Feature -eq $feature.Name })
    $featureFunctions = @(
        $featureReferences |
        Where-Object { $null -ne $_.FunctionBeginRva } |
        Sort-Object FunctionBeginRva, InstructionRva
    )

    $anchorLines = if ($featureAnchors.Count -gt 0) {
        $featureAnchors | Select-Object -First 20 | ForEach-Object {
            "AnchorRva=0x$('{0:X8}' -f $_.RVA) | Section=$($_.Section) | Match=$($_.Match) | Text=$($_.Text)"
        }
    } else {
        @('No anchors found.')
    }

    $functionLines = if ($featureFunctions.Count -gt 0) {
        $featureFunctions |
        Group-Object FunctionBeginRva,FunctionEndRva,FunctionSection |
        Select-Object -First 20 |
        ForEach-Object {
            $firstReference = $_.Group | Select-Object -First 1
            $anchorMatches = ($_.Group | Select-Object -ExpandProperty AnchorMatch -Unique)
            $anchorTexts = ($_.Group | Select-Object -ExpandProperty AnchorText -Unique | Select-Object -First 3)
            "Function=0x$('{0:X8}' -f [uint32]$firstReference.FunctionBeginRva)-0x$('{0:X8}' -f [uint32]$firstReference.FunctionEndRva) | Section=$($firstReference.FunctionSection) | RefCount=$($_.Count) | Matches=$([string]::Join(', ', $anchorMatches)) | Anchors=$([string]::Join(' || ', $anchorTexts))"
        }
    } else {
        @('No RIP-relative anchor references found in executable sections.')
    }

    Add-Section -Lines $lines -Title "$($feature.Name) Anchors" -Body $anchorLines

    Add-Section -Lines $lines -Title "$($feature.Name) Candidate Functions" -Body $functionLines
}

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This feature map is heuristic. It groups likely feature regions by anchor strings and RIP-relative references, not by symbols.',
    'It is intended to narrow offline reverse-engineering scope before any host-stack experiment is attempted.',
    'A missing function cluster for a category does not prove the category is absent; it only means this pass did not find a direct RIP-relative string reference to its anchors.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "USBXHCI feature map written to $resolvedOutputPath"
