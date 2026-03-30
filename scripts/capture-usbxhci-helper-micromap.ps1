[CmdletBinding()]
param(
    [string]$ImagePath,
    [uint32[]]$HelperRvas = @(
        [uint32]0x00006BA0,
        [uint32]0x00010440,
        [uint32]0x00004124
    ),
    [int]$NeighborRadius = 2,
    [int]$SecondHopTargets = 4,
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
    return (Join-Path $defaultDir 'usbxhci-helper-micromap.txt')
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
                SourceBeginRva    = $Function.BeginRva
                SourceEndRva      = $Function.EndRva
                SourceSection     = $Function.Section
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
                SourceBeginRva    = $Function.BeginRva
                SourceEndRva      = $Function.EndRva
                SourceSection     = $Function.Section
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
$imports = Parse-ImportsWithIat -Bytes $bytes -Layout $layout

$selectedHelpers = New-Object System.Collections.Generic.List[object]
foreach ($helperRva in $HelperRvas) {
    $function = $functions | Where-Object { $_.BeginRva -eq $helperRva } | Select-Object -First 1
    if (-not $function) {
        continue
    }

    $selectedHelpers.Add($function) | Out-Null
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Helper Micro Map') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "ImagePath          : $resolvedImagePath",
    "FileVersion        : $($file.VersionInfo.FileVersion)",
    "ProductVersion     : $($file.VersionInfo.ProductVersion)",
    "TimeDateStamp      : 0x$('{0:X8}' -f $layout.TimeDateStamp) ($(Convert-TimeDateStamp -TimeDateStamp $layout.TimeDateStamp))",
    "RequestedHelpers   : $([string]::Join(', ', ($HelperRvas | ForEach-Object { '0x{0:X8}' -f $_ })))",
    "ResolvedHelpers    : $($selectedHelpers.Count)",
    "NeighborRadius     : $NeighborRadius",
    "SecondHopTargets   : $SecondHopTargets"
)

foreach ($helper in $selectedHelpers) {
    $unwindSummary = Parse-UnwindInfoSummary -Bytes $bytes -Layout $layout -UnwindRva $helper.UnwindRva
    $neighbors = Get-NeighborFunctions -Function $helper -Functions $functions -Radius $NeighborRadius
    $firstHopSites = Find-DirectCallSitesForFunction -Bytes $bytes -Layout $layout -Function $helper -Functions $functions -Imports $imports
    $firstHopInternal = @($firstHopSites | Where-Object { $_.InstructionKind -eq 'CALL_REL' })
    $firstHopIat = @($firstHopSites | Where-Object { $_.InstructionKind -ne 'CALL_REL' })
    $topSecondHopSeeds = @(
        $firstHopInternal |
        Group-Object TargetFunctionRva |
        Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
        Select-Object -First $SecondHopTargets
    )

    Add-Section -Lines $lines -Title ("Primary Helper {0}" -f (Format-FunctionLabel -Function $helper)) -Body @(
        "Section            : $($helper.Section)",
        "Size               : $($helper.Size)",
        "UnwindRva          : 0x$('{0:X8}' -f $helper.UnwindRva)",
        "UnwindInfo         : $unwindSummary",
        "FirstHopInternal   : $($firstHopInternal.Count)",
        "FirstHopIat        : $($firstHopIat.Count)",
        "SecondHopSeedCount : $($topSecondHopSeeds.Count)"
    )

    $neighborLines = @(
        $neighbors | ForEach-Object {
            $neighbor = $_.Function
            $neighborUnwind = Parse-UnwindInfoSummary -Bytes $bytes -Layout $layout -UnwindRva $neighbor.UnwindRva
            $marker = if ($_.RelativeIndex -eq 0) { '<primary>' } elseif ($_.RelativeIndex -lt 0) { ('prev{0}' -f ([Math]::Abs($_.RelativeIndex))) } else { ('next{0}' -f $_.RelativeIndex) }
            "Slot=$marker | Function=$(Format-FunctionLabel -Function $neighbor) | Section=$($neighbor.Section) | Size=$($neighbor.Size) | Unwind=0x$('{0:X8}' -f $neighbor.UnwindRva) | UnwindInfo=$neighborUnwind"
        }
    )
    Add-Section -Lines $lines -Title ("Neighbors 0x{0:X8}" -f $helper.BeginRva) -Body $neighborLines

    $firstHopInternalLines = if ($firstHopInternal.Count -gt 0) {
        $firstHopInternal |
        Group-Object TargetFunctionRva |
        Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
        Select-Object -First 12 |
        ForEach-Object {
            $first = $_.Group | Select-Object -First 1
            "Target=0x$('{0:X8}' -f [uint32]$_.Name) | Calls=$($_.Count) | TargetSection=$($first.TargetSection) | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
        }
    } else {
        @('No direct relative internal calls captured.')
    }
    Add-Section -Lines $lines -Title ("First-Hop Internal 0x{0:X8}" -f $helper.BeginRva) -Body $firstHopInternalLines

    $firstHopIatLines = if ($firstHopIat.Count -gt 0) {
        $firstHopIat |
        Group-Object TargetLabel |
        Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
        Select-Object -First 12 |
        ForEach-Object {
            $first = $_.Group | Select-Object -First 1
            "Import=$($_.Name) | Calls=$($_.Count) | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
        }
    } else {
        @('No direct RIP-relative IAT calls captured.')
    }
    Add-Section -Lines $lines -Title ("First-Hop IAT 0x{0:X8}" -f $helper.BeginRva) -Body $firstHopIatLines

    foreach ($seed in $topSecondHopSeeds) {
        $targetRva = [uint32]$seed.Name
        $secondHopFunction = $functions | Where-Object { $_.BeginRva -eq $targetRva } | Select-Object -First 1
        if (-not $secondHopFunction) {
            continue
        }

        $secondHopUnwind = Parse-UnwindInfoSummary -Bytes $bytes -Layout $layout -UnwindRva $secondHopFunction.UnwindRva
        $secondHopSites = Find-DirectCallSitesForFunction -Bytes $bytes -Layout $layout -Function $secondHopFunction -Functions $functions -Imports $imports
        $secondHopInternal = @($secondHopSites | Where-Object { $_.InstructionKind -eq 'CALL_REL' })
        $secondHopIat = @($secondHopSites | Where-Object { $_.InstructionKind -ne 'CALL_REL' })

        Add-Section -Lines $lines -Title ("Second-Hop Target 0x{0:X8} from 0x{1:X8}" -f $secondHopFunction.BeginRva, $helper.BeginRva) -Body @(
            "Function           : $(Format-FunctionLabel -Function $secondHopFunction)",
            "Section            : $($secondHopFunction.Section)",
            "Size               : $($secondHopFunction.Size)",
            "UnwindRva          : 0x$('{0:X8}' -f $secondHopFunction.UnwindRva)",
            "UnwindInfo         : $secondHopUnwind",
            "InboundFromPrimary : $($seed.Count)",
            "SecondHopInternal  : $($secondHopInternal.Count)",
            "SecondHopIat       : $($secondHopIat.Count)"
        )

        $secondHopInternalLines = if ($secondHopInternal.Count -gt 0) {
            $secondHopInternal |
            Group-Object TargetFunctionRva |
            Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
            Select-Object -First 8 |
            ForEach-Object {
                $first = $_.Group | Select-Object -First 1
                "Target=0x$('{0:X8}' -f [uint32]$_.Name) | Calls=$($_.Count) | TargetSection=$($first.TargetSection) | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
            }
        } else {
            @('No direct relative internal calls captured.')
        }
        Add-Section -Lines $lines -Title ("Second-Hop Internal 0x{0:X8}" -f $secondHopFunction.BeginRva) -Body $secondHopInternalLines

        $secondHopIatLines = if ($secondHopIat.Count -gt 0) {
            $secondHopIat |
            Group-Object TargetLabel |
            Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
            Select-Object -First 8 |
            ForEach-Object {
                $first = $_.Group | Select-Object -First 1
                "Import=$($_.Name) | Calls=$($_.Count) | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
            }
        } else {
            @('No direct RIP-relative IAT calls captured.')
        }
        Add-Section -Lines $lines -Title ("Second-Hop IAT 0x{0:X8}" -f $secondHopFunction.BeginRva) -Body $secondHopIatLines
    }
}

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only and intentionally tiny. It focuses only on the three upstream helper bodies identified by the previous helper convergence map.',
    'Neighbors come from adjacent runtime-function entries in the current .pdata ordering. Internal edges are direct E8 relative calls only. Imported edges are direct RIP-relative CALL/JMP IAT sites only.',
    'The output is intended to show whether the helper tier keeps converging into a small dispatch nucleus or fans back out into multiple subsystems.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "USBXHCI helper micro map written to $resolvedOutputPath"
