[CmdletBinding()]
param(
    [string]$ImagePath,
    [string]$OutputPath,
    [string[]]$Keywords = @(
        'USBXHCI',
        'Endpoint',
        'Interval',
        'Interrupt',
        'Transfer',
        'Isoch',
        'Doorbell',
        'Ring',
        'TRB',
        'Poll',
        'Burst'
    ),
    [int]$MinStringLength = 6,
    [int]$MaxKeywordHits = 40
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
    return (Join-Path $defaultDir 'usbxhci-symbol-recon.txt')
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

function Get-Sha256Hex {
    param([byte[]]$Bytes)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha256.ComputeHash($Bytes))).Replace('-', '')
    } finally {
        $sha256.Dispose()
    }
}

function Format-HexBytes {
    param(
        [byte[]]$Bytes,
        [int]$Offset,
        [int]$Count
    )

    $slice = Get-ByteSlice -Bytes $Bytes -Offset $Offset -Length ([Math]::Min($Count, $Bytes.Length - $Offset))
    if ($slice.Count -eq 0) {
        return ''
    }

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

    $epoch = [DateTimeOffset]::FromUnixTimeSeconds([int64]$TimeDateStamp)
    return $epoch.ToString('yyyy-MM-dd HH:mm:ss zzz')
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

function Get-SubsystemName {
    param([uint16]$Subsystem)

    switch ($Subsystem) {
        1 { return 'Native' }
        2 { return 'Windows GUI' }
        3 { return 'Windows CUI' }
        9 { return 'Windows CE' }
        10 { return 'EFI Application' }
        11 { return 'EFI Boot Service Driver' }
        12 { return 'EFI Runtime Driver' }
        default { return ('0x{0:X4}' -f $Subsystem) }
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

function Format-DllCharacteristics {
    param([uint16]$Flags)

    $names = New-Object System.Collections.Generic.List[string]
    if ($Flags -band 0x0020) { $names.Add('HIGH_ENTROPY_VA') | Out-Null }
    if ($Flags -band 0x0040) { $names.Add('DYNAMIC_BASE') | Out-Null }
    if ($Flags -band 0x0080) { $names.Add('FORCE_INTEGRITY') | Out-Null }
    if ($Flags -band 0x0100) { $names.Add('NX_COMPAT') | Out-Null }
    if ($Flags -band 0x0200) { $names.Add('NO_ISOLATION') | Out-Null }
    if ($Flags -band 0x0400) { $names.Add('NO_SEH') | Out-Null }
    if ($Flags -band 0x0800) { $names.Add('NO_BIND') | Out-Null }
    if ($Flags -band 0x1000) { $names.Add('APPCONTAINER') | Out-Null }
    if ($Flags -band 0x2000) { $names.Add('WDM_DRIVER') | Out-Null }
    if ($Flags -band 0x4000) { $names.Add('GUARD_CF') | Out-Null }
    if ($Flags -band 0x8000) { $names.Add('TERMINAL_SERVER_AWARE') | Out-Null }
    if ($names.Count -eq 0) {
        return '<none>'
    }

    return ($names -join ', ')
}

function Parse-PeLayout {
    param([byte[]]$Bytes)

    if ($Bytes.Length -lt 0x100) {
        throw 'File too small to be a PE image.'
    }

    if ([System.Text.Encoding]::ASCII.GetString($Bytes, 0, 2) -ne 'MZ') {
        throw 'DOS header signature mismatch.'
    }

    $peOffset = [int](Read-UInt32LE -Bytes $Bytes -Offset 0x3C)
    if ([System.Text.Encoding]::ASCII.GetString($Bytes, $peOffset, 4) -ne 'PE' + [char]0 + [char]0) {
        throw 'PE signature mismatch.'
    }

    $coffOffset = $peOffset + 4
    $machine = Read-UInt16LE -Bytes $Bytes -Offset $coffOffset
    $sectionCount = Read-UInt16LE -Bytes $Bytes -Offset ($coffOffset + 2)
    $timeDateStamp = Read-UInt32LE -Bytes $Bytes -Offset ($coffOffset + 4)
    $sizeOfOptionalHeader = Read-UInt16LE -Bytes $Bytes -Offset ($coffOffset + 16)
    $characteristics = Read-UInt16LE -Bytes $Bytes -Offset ($coffOffset + 18)

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
    $subsystem = Read-UInt16LE -Bytes $Bytes -Offset ($optionalHeaderOffset + 68)
    $dllCharacteristics = Read-UInt16LE -Bytes $Bytes -Offset ($optionalHeaderOffset + 70)
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
        $sectionCharacteristics = Read-UInt32LE -Bytes $Bytes -Offset ($sectionOffset + 36)
        $rawSlice = if ($sizeOfRawData -gt 0) {
            Get-ByteSlice -Bytes $Bytes -Offset $pointerToRawData -Length $sizeOfRawData
        } else {
            @()
        }

        $sections += [pscustomobject]@{
            Name            = $name
            VirtualSize     = $virtualSize
            VirtualAddress  = $virtualAddress
            SizeOfRawData   = $sizeOfRawData
            PointerToRawData = $pointerToRawData
            Characteristics = $sectionCharacteristics
            Flags           = Format-SectionFlags -Characteristics $sectionCharacteristics
            RawHashSha256   = if ($rawSlice.Count -gt 0) { Get-Sha256Hex -Bytes $rawSlice } else { $null }
        }
    }

    return [pscustomobject]@{
        PeOffset              = $peOffset
        Machine               = $machine
        SectionCount          = $sectionCount
        TimeDateStamp         = $timeDateStamp
        Characteristics       = $characteristics
        IsPe32Plus            = $isPe32Plus
        EntryPointRva         = $entryPointRva
        ImageBase             = $imageBase
        SizeOfImage           = $sizeOfImage
        SizeOfHeaders         = $sizeOfHeaders
        Subsystem             = $subsystem
        DllCharacteristics    = $dllCharacteristics
        DataDirectories       = $dataDirectories
        Sections              = $sections
    }
}

function Get-DataDirectory {
    param(
        [object]$Layout,
        [int]$Index
    )

    return ($Layout.DataDirectories | Where-Object { $_.Index -eq $Index } | Select-Object -First 1)
}

function Parse-DebugDirectory {
    param(
        [byte[]]$Bytes,
        [object]$Layout
    )

    $directory = Get-DataDirectory -Layout $Layout -Index 6
    if (-not $directory -or $directory.RVA -eq 0 -or $directory.Size -eq 0) {
        return @()
    }

    $directoryOffset = Convert-RvaToOffset -Rva $directory.RVA -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
    if ($null -eq $directoryOffset) {
        return @()
    }

    $entries = @()
    $entryCount = [int]($directory.Size / 28)
    for ($index = 0; $index -lt $entryCount; $index++) {
        $entryOffset = $directoryOffset + ($index * 28)
        if (($entryOffset + 28) -gt $Bytes.Length) {
            break
        }

        $type = Read-UInt32LE -Bytes $Bytes -Offset ($entryOffset + 12)
        $sizeOfData = Read-UInt32LE -Bytes $Bytes -Offset ($entryOffset + 16)
        $addressOfRawData = Read-UInt32LE -Bytes $Bytes -Offset ($entryOffset + 20)
        $pointerToRawData = Read-UInt32LE -Bytes $Bytes -Offset ($entryOffset + 24)
        $record = [pscustomobject]@{
            Type           = $type
            SizeOfData     = $sizeOfData
            AddressOfRawData = $addressOfRawData
            PointerToRawData = $pointerToRawData
            CodeViewFormat = $null
            PdbPath        = $null
            Guid           = $null
            Age            = $null
        }

        if ($type -eq 2 -and $sizeOfData -ge 24 -and ($pointerToRawData + 24) -le $Bytes.Length) {
            $signature = [System.Text.Encoding]::ASCII.GetString($Bytes, $pointerToRawData, 4)
            $record.CodeViewFormat = $signature
            if ($signature -eq 'RSDS') {
                $guidBytes = Get-ByteSlice -Bytes $Bytes -Offset ($pointerToRawData + 4) -Length 16
                try {
                    $record.Guid = [System.Guid]::new($guidBytes).ToString()
                } catch {
                    $record.Guid = 'RAW:' + (($guidBytes | ForEach-Object { $_.ToString('X2') }) -join '')
                }
                $record.Age = Read-UInt32LE -Bytes $Bytes -Offset ($pointerToRawData + 20)
                $record.PdbPath = Read-AsciiZ -Bytes $Bytes -Offset ($pointerToRawData + 24)
            }
        }

        $entries += $record
    }

    return $entries
}

function Parse-Imports {
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
    $thunkSize = if ($Layout.IsPe32Plus) { 8 } else { 4 }
    $ordinalMask = if ($Layout.IsPe32Plus) { ([uint64]1 -shl 63) } else { [uint64]0x80000000 }

    for ($descriptorIndex = 0; ; $descriptorIndex++) {
        $offset = $descriptorOffset + ($descriptorIndex * 20)
        if (($offset + 20) -gt $Bytes.Length) {
            break
        }

        $originalFirstThunk = Read-UInt32LE -Bytes $Bytes -Offset $offset
        $timeDateStamp = Read-UInt32LE -Bytes $Bytes -Offset ($offset + 4)
        $forwarderChain = Read-UInt32LE -Bytes $Bytes -Offset ($offset + 8)
        $nameRva = Read-UInt32LE -Bytes $Bytes -Offset ($offset + 12)
        $firstThunk = Read-UInt32LE -Bytes $Bytes -Offset ($offset + 16)

        if (($originalFirstThunk -bor $timeDateStamp -bor $forwarderChain -bor $nameRva -bor $firstThunk) -eq 0) {
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

        $importNames = New-Object System.Collections.Generic.List[string]
        $ordinalCount = 0
        for ($thunkIndex = 0; ; $thunkIndex++) {
            $thunkOffset = $lookupOffset + ($thunkIndex * $thunkSize)
            if (($thunkOffset + $thunkSize) -gt $Bytes.Length) {
                break
            }

            $thunkValue = if ($Layout.IsPe32Plus) {
                Read-UInt64LE -Bytes $Bytes -Offset $thunkOffset
            } else {
                [uint64](Read-UInt32LE -Bytes $Bytes -Offset $thunkOffset)
            }

            if ($thunkValue -eq 0) {
                break
            }

            if (($thunkValue -band $ordinalMask) -ne 0) {
                $ordinalCount++
                continue
            }

            $hintNameRva = [uint32]$thunkValue
            $hintNameOffset = Convert-RvaToOffset -Rva $hintNameRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
            if ($null -eq $hintNameOffset) {
                continue
            }

            $importName = Read-AsciiZ -Bytes $Bytes -Offset ($hintNameOffset + 2)
            if (-not [string]::IsNullOrWhiteSpace($importName)) {
                $importNames.Add($importName) | Out-Null
            }
        }

        $imports += [pscustomobject]@{
            DllName        = $dllName
            ImportCount    = $importNames.Count + $ordinalCount
            NamedCount     = $importNames.Count
            OrdinalCount   = $ordinalCount
            SampleImports  = ($importNames | Select-Object -First 12)
        }
    }

    return $imports
}

function Parse-Exports {
    param(
        [byte[]]$Bytes,
        [object]$Layout
    )

    $directory = Get-DataDirectory -Layout $Layout -Index 0
    if (-not $directory -or $directory.RVA -eq 0 -or $directory.Size -eq 0) {
        return @()
    }

    $exportOffset = Convert-RvaToOffset -Rva $directory.RVA -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
    if ($null -eq $exportOffset -or ($exportOffset + 40) -gt $Bytes.Length) {
        return @()
    }

    $nameRva = Read-UInt32LE -Bytes $Bytes -Offset ($exportOffset + 12)
    $ordinalBase = Read-UInt32LE -Bytes $Bytes -Offset ($exportOffset + 16)
    $numberOfNames = Read-UInt32LE -Bytes $Bytes -Offset ($exportOffset + 24)
    $addressOfNamesRva = Read-UInt32LE -Bytes $Bytes -Offset ($exportOffset + 32)
    $addressOfNameOrdinalsRva = Read-UInt32LE -Bytes $Bytes -Offset ($exportOffset + 36)

    $exportName = $null
    $nameOffset = Convert-RvaToOffset -Rva $nameRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
    if ($null -ne $nameOffset) {
        $exportName = Read-AsciiZ -Bytes $Bytes -Offset $nameOffset
    }

    $namesOffset = Convert-RvaToOffset -Rva $addressOfNamesRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
    $ordinalsOffset = Convert-RvaToOffset -Rva $addressOfNameOrdinalsRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
    if ($null -eq $namesOffset -or $null -eq $ordinalsOffset) {
        return @()
    }

    $exports = @()
    for ($index = 0; $index -lt $numberOfNames; $index++) {
        $nameEntryOffset = $namesOffset + ($index * 4)
        $ordinalEntryOffset = $ordinalsOffset + ($index * 2)
        if (($nameEntryOffset + 4) -gt $Bytes.Length -or ($ordinalEntryOffset + 2) -gt $Bytes.Length) {
            break
        }

        $exportNameRva = Read-UInt32LE -Bytes $Bytes -Offset $nameEntryOffset
        $exportNameOffset = Convert-RvaToOffset -Rva $exportNameRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
        if ($null -eq $exportNameOffset) {
            continue
        }

        $functionName = Read-AsciiZ -Bytes $Bytes -Offset $exportNameOffset
        $ordinal = $ordinalBase + (Read-UInt16LE -Bytes $Bytes -Offset $ordinalEntryOffset)
        $exports += [pscustomobject]@{
            ModuleName = $exportName
            Name       = $functionName
            Ordinal    = $ordinal
        }
    }

    return $exports
}

function Find-KeywordStrings {
    param(
        [byte[]]$Bytes,
        [string[]]$Keywords,
        [int]$MinLength,
        [int]$MaxHits
    )

    $hits = New-Object System.Collections.Generic.List[object]
    $keywordRegex = '(?i)(' + (($Keywords | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')'
    $start = -1

    for ($index = 0; $index -lt $Bytes.Length; $index++) {
        $byte = $Bytes[$index]
        $isPrintable = ($byte -ge 0x20 -and $byte -le 0x7E)
        if ($isPrintable) {
            if ($start -lt 0) {
                $start = $index
            }
            continue
        }

        if ($start -ge 0) {
            $length = $index - $start
            if ($length -ge $MinLength) {
                $text = [System.Text.Encoding]::ASCII.GetString($Bytes, $start, $length)
                $match = [regex]::Match($text, $keywordRegex)
                if ($match.Success) {
                    $hits.Add([pscustomobject]@{
                        Offset  = $start
                        Keyword = $match.Groups[1].Value
                        Text    = $text
                    }) | Out-Null
                    if ($hits.Count -ge $MaxHits) {
                        break
                    }
                }
            }
            $start = -1
        }
    }

    return $hits
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
$signature = Get-AuthenticodeSignature -FilePath $resolvedImagePath
$layout = Parse-PeLayout -Bytes $bytes
$debugEntries = Parse-DebugDirectory -Bytes $bytes -Layout $layout
$imports = Parse-Imports -Bytes $bytes -Layout $layout
$exports = Parse-Exports -Bytes $bytes -Layout $layout
$keywordHits = Find-KeywordStrings -Bytes $bytes -Keywords $Keywords -MinLength $MinStringLength -MaxHits $MaxKeywordHits
$entryPointOffset = Convert-RvaToOffset -Rva $layout.EntryPointRva -Sections $layout.Sections -SizeOfHeaders $layout.SizeOfHeaders
$authenticodeSigner = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '<none>' }
$symbolPathEnvironment = if ($env:_NT_SYMBOL_PATH) { $env:_NT_SYMBOL_PATH } else { '<not set>' }
$peFormat = if ($layout.IsPe32Plus) { 'PE32+' } else { 'PE32' }
$entryPointFileOffsetText = if ($null -ne $entryPointOffset) { ('0x{0:X8}' -f $entryPointOffset) } else { '<unmapped>' }
$entryPointBytesText = if ($null -ne $entryPointOffset) { Format-HexBytes -Bytes $bytes -Offset $entryPointOffset -Count 16 } else { '<unmapped>' }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Symbol And Pattern Recon') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "ImagePath             : $resolvedImagePath",
    "FileVersion           : $($file.VersionInfo.FileVersion)",
    "ProductVersion        : $($file.VersionInfo.ProductVersion)",
    "LengthBytes           : $($bytes.Length)",
    "SHA256                : $((Get-FileHash -Path $resolvedImagePath -Algorithm SHA256).Hash)",
    "AuthenticodeStatus    : $($signature.Status)",
    "AuthenticodeSigner    : $authenticodeSigner",
    "SymbolPathEnvironment : $symbolPathEnvironment"
)

Add-Section -Lines $lines -Title 'PE Headers' -Body @(
    "Machine                : $(Get-MachineName -Machine $layout.Machine)",
    "TimeDateStamp          : 0x$('{0:X8}' -f $layout.TimeDateStamp) ($([string](Convert-TimeDateStamp -TimeDateStamp $layout.TimeDateStamp)))",
    "SectionCount           : $($layout.SectionCount)",
    "PEFormat               : $peFormat",
    "ImageBase              : 0x$('{0:X}' -f $layout.ImageBase)",
    "EntryPointRva          : 0x$('{0:X8}' -f $layout.EntryPointRva)",
    "EntryPointFileOffset   : $entryPointFileOffsetText",
    "EntryPointBytes16      : $entryPointBytesText",
    "SizeOfImage            : 0x$('{0:X8}' -f $layout.SizeOfImage)",
    "SizeOfHeaders          : 0x$('{0:X8}' -f $layout.SizeOfHeaders)",
    "Subsystem              : $(Get-SubsystemName -Subsystem $layout.Subsystem)",
    "DllCharacteristics     : 0x$('{0:X4}' -f $layout.DllCharacteristics)",
    "DllCharacteristicFlags : $(Format-DllCharacteristics -Flags $layout.DllCharacteristics)"
)

if ($debugEntries.Count -gt 0) {
    Add-Section -Lines $lines -Title 'Debug Directory' -Body @(
        ($debugEntries | ForEach-Object {
            "Type=$($_.Type) Size=0x$('{0:X}' -f $_.SizeOfData) Raw=0x$('{0:X8}' -f $_.PointerToRawData) CodeView=$($_.CodeViewFormat) Guid=$($_.Guid) Age=$($_.Age) PdbPath=$($_.PdbPath)"
        })
    )
}

Add-Section -Lines $lines -Title 'Section Anchors' -Body @(
    ($layout.Sections | ForEach-Object {
        "$($_.Name) | RVA=0x$('{0:X8}' -f $_.VirtualAddress) | VSz=0x$('{0:X8}' -f $_.VirtualSize) | RawSz=0x$('{0:X8}' -f $_.SizeOfRawData) | Raw=0x$('{0:X8}' -f $_.PointerToRawData) | Flags=$($_.Flags) | SHA256=$($_.RawHashSha256)"
    })
)

if ($imports.Count -gt 0) {
    Add-Section -Lines $lines -Title 'Imports' -Body @(
        ($imports | Sort-Object DllName | ForEach-Object {
            "$($_.DllName) | Total=$($_.ImportCount) | Named=$($_.NamedCount) | Ordinal=$($_.OrdinalCount) | Sample=$([string]::Join(', ', $_.SampleImports))"
        })
    )
}

if ($exports.Count -gt 0) {
    Add-Section -Lines $lines -Title 'Exports' -Body @(
        ($exports | Sort-Object Ordinal | Select-Object -First 64 | ForEach-Object {
            "$($_.ModuleName) | Ordinal=$($_.Ordinal) | Name=$($_.Name)"
        })
    )
} else {
    Add-Section -Lines $lines -Title 'Exports' -Body @('No named exports parsed from the image.')
}

if ($keywordHits.Count -gt 0) {
    Add-Section -Lines $lines -Title 'Keyword Strings' -Body @(
        ($keywordHits | ForEach-Object {
            "Offset=0x$('{0:X8}' -f $_.Offset) | Keyword=$($_.Keyword) | Text=$($_.Text)"
        })
    )
} else {
    Add-Section -Lines $lines -Title 'Keyword Strings' -Body @(
        "No ASCII keyword hits were found for: $([string]::Join(', ', $Keywords))"
    )
}

Add-Section -Lines $lines -Title 'Risk Notes' -Body @(
    'This pass is read-only. It does not patch, map, or load alternate host-stack code.',
    'The output is intended to identify stable image anchors and recon signals before any host-stack experiment is considered.',
    'Image-specific patch logic should not be assumed transferable across USBXHCI builds without fresh analysis.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "USBXHCI symbol/pattern recon written to $resolvedOutputPath"
