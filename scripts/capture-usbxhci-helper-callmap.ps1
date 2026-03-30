[CmdletBinding()]
param(
    [string]$ImagePath,
    [string]$TargetedCallMapPath,
    [uint32[]]$HelperRvas,
    [int]$TopHelpers = 6,
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

function Resolve-TargetedCallMapPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return (Join-Path (Join-Path (Get-RepoRoot) 'out\dev') 'usbxhci-targeted-callmap.txt')
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    $defaultDir = Join-Path (Get-RepoRoot) 'out\dev'
    New-Item -ItemType Directory -Force -Path $defaultDir | Out-Null
    return (Join-Path $defaultDir 'usbxhci-helper-callmap.txt')
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

function Find-HelperByRva {
    param(
        [uint32]$Rva,
        [object[]]$Helpers
    )

    foreach ($helper in $Helpers) {
        if ($Rva -ge $helper.BeginRva -and $Rva -lt $helper.EndRva) {
            return $helper
        }
    }

    return $null
}

function Parse-TransferHelperSeeds {
    param(
        [string]$Path,
        [uint32[]]$ExplicitHelperRvas,
        [int]$TopHelpers
    )

    if (-not (Test-Path $Path)) {
        throw "Targeted call map not found: $Path"
    }

    $aggregates = @{}
    $currentTransferSource = $null
    foreach ($line in Get-Content $Path) {
        if ($line -match '^## Internal Calls Transfer 0x(?<source>[0-9A-Fa-f]+)$') {
            $currentTransferSource = [uint32]('0x' + $matches['source'])
            continue
        }

        if ($line -match '^## ') {
            $currentTransferSource = $null
            continue
        }

        if ($null -eq $currentTransferSource) {
            continue
        }

        if ($line -match '^Target=0x(?<target>[0-9A-Fa-f]+) \| Calls=(?<count>\d+) \| TargetSection=(?<section>[^|]+) \| TargetFeature=(?<feature>[^|]+) ') {
            $targetRva = [uint32]('0x' + $matches['target'])
            $targetSection = $matches['section'].Trim()
            $targetFeature = $matches['feature'].Trim()
            if ($targetSection -eq '<unmapped>' -or $targetFeature -ne '<none>') {
                continue
            }

            if (-not $aggregates.ContainsKey($targetRva)) {
                $aggregates[$targetRva] = [pscustomobject]@{
                    BeginRva      = $targetRva
                    TotalCalls    = 0
                    CallerSet     = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
                    SourceSamples  = New-Object System.Collections.Generic.List[string]
                }
            }

            $entry = $aggregates[$targetRva]
            $entry.TotalCalls += [int]$matches['count']
            $sourceLabel = ('0x{0:X8}' -f $currentTransferSource)
            [void]$entry.CallerSet.Add($sourceLabel)
            if ($entry.SourceSamples.Count -lt 8 -and -not ($entry.SourceSamples -contains $sourceLabel)) {
                $entry.SourceSamples.Add($sourceLabel) | Out-Null
            }
        }
    }

    $allSeeds = @(
        $aggregates.Values |
        Sort-Object -Property @{ Expression = 'TotalCalls'; Descending = $true }, @{ Expression = { $_.CallerSet.Count }; Descending = $true }, 'BeginRva'
    )

    if ($ExplicitHelperRvas -and $ExplicitHelperRvas.Count -gt 0) {
        $selected = New-Object System.Collections.Generic.List[object]
        foreach ($explicitRva in $ExplicitHelperRvas) {
            if ($aggregates.ContainsKey([uint32]$explicitRva)) {
                $selected.Add($aggregates[[uint32]$explicitRva]) | Out-Null
            } else {
                $selected.Add([pscustomobject]@{
                    BeginRva      = [uint32]$explicitRva
                    TotalCalls    = 0
                    CallerSet     = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
                    SourceSamples = New-Object System.Collections.Generic.List[string]
                }) | Out-Null
            }
        }
        return [pscustomobject]@{
            Selected = @($selected)
            AllSeeds = $allSeeds
        }
    }

    return [pscustomobject]@{
        Selected = @($allSeeds | Select-Object -First $TopHelpers)
        AllSeeds = $allSeeds
    }
}

function Resolve-HelperFunctions {
    param(
        [object[]]$Seeds,
        [object[]]$Functions
    )

    $helpers = New-Object System.Collections.Generic.List[object]
    foreach ($seed in $Seeds) {
        $function = $Functions | Where-Object { $_.BeginRva -eq $seed.BeginRva } | Select-Object -First 1
        if (-not $function) {
            continue
        }

        $helpers.Add([pscustomobject]@{
            BeginRva        = $function.BeginRva
            EndRva          = $function.EndRva
            Section         = $function.Section
            UnwindRva       = $function.UnwindRva
            TotalInbound    = $seed.TotalCalls
            UniqueCallers   = $seed.CallerSet.Count
            CallerSamples   = @($seed.SourceSamples)
        }) | Out-Null
    }

    return @($helpers | Sort-Object BeginRva)
}

function Find-HelperCallSites {
    param(
        [byte[]]$Bytes,
        [object]$Layout,
        [object[]]$Helpers,
        [object[]]$Functions,
        [object[]]$Imports
    )

    $importsByIatRva = @{}
    foreach ($import in $Imports) {
        $importsByIatRva[[uint32]$import.IatRva] = $import
    }

    $sites = New-Object System.Collections.Generic.List[object]
    foreach ($helper in $Helpers) {
        $offset = Convert-RvaToOffset -Rva $helper.BeginRva -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
        if ($null -eq $offset) {
            continue
        }

        $length = [int]($helper.EndRva - $helper.BeginRva)
        $endOffset = [Math]::Min($offset + $length, $Bytes.Length)
        for ($cursor = $offset; $cursor -lt $endOffset; $cursor++) {
            $instructionRva = [uint32]($helper.BeginRva + ($cursor - $offset))

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

                $calleeHelper = Find-HelperByRva -Rva $calleeRva -Helpers $Helpers
                $sites.Add([pscustomobject]@{
                    SourceBeginRva    = $helper.BeginRva
                    SourceEndRva      = $helper.EndRva
                    SourceSection     = $helper.Section
                    InstructionKind   = 'CALL_REL'
                    InstructionRva    = $instructionRva
                    InstructionHex    = Format-HexBytes -Bytes $Bytes -Offset $cursor -Count 5
                    TargetFunctionRva = $calleeFunction.BeginRva
                    TargetFunctionEnd = $calleeFunction.EndRva
                    TargetSection     = $calleeFunction.Section
                    TargetHelper      = if ($calleeHelper) { ('0x{0:X8}' -f $calleeHelper.BeginRva) } else { $null }
                    TargetLabel       = if ($calleeHelper) { ('Helper:0x{0:X8}' -f $calleeHelper.BeginRva) } else { ('0x{0:X8}' -f $calleeFunction.BeginRva) }
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
                    SourceBeginRva    = $helper.BeginRva
                    SourceEndRva      = $helper.EndRva
                    SourceSection     = $helper.Section
                    InstructionKind   = if ($Bytes[$cursor + 1] -eq 0x15) { 'CALL_IAT' } else { 'JMP_IAT' }
                    InstructionRva    = $instructionRva
                    InstructionHex    = Format-HexBytes -Bytes $Bytes -Offset $cursor -Count 6
                    TargetFunctionRva = $null
                    TargetFunctionEnd = $null
                    TargetSection     = '.idata'
                    TargetHelper      = $null
                    TargetLabel       = ('{0}!{1}' -f $import.DllName, $import.ImportName)
                    ImportName        = ('{0}!{1}' -f $import.DllName, $import.ImportName)
                }) | Out-Null
            }
        }
    }

    return @($sites | Sort-Object SourceBeginRva, InstructionRva)
}

$resolvedImagePath = Resolve-ImagePath -RequestedPath $ImagePath
if (-not (Test-Path $resolvedImagePath)) {
    throw "USBXHCI image not found: $resolvedImagePath"
}

$resolvedTargetedCallMapPath = Resolve-TargetedCallMapPath -RequestedPath $TargetedCallMapPath
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
$seedResult = Parse-TransferHelperSeeds -Path $resolvedTargetedCallMapPath -ExplicitHelperRvas $HelperRvas -TopHelpers $TopHelpers
$helperFunctions = Resolve-HelperFunctions -Seeds $seedResult.Selected -Functions $functions
$helperCallSites = Find-HelperCallSites -Bytes $bytes -Layout $layout -Helpers $helperFunctions -Functions $functions -Imports $imports

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# USBXHCI Helper Call Map') | Out-Null
$lines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$lines.Add('') | Out-Null

Add-Section -Lines $lines -Title 'Summary' -Body @(
    "ImagePath            : $resolvedImagePath",
    "FileVersion          : $($file.VersionInfo.FileVersion)",
    "ProductVersion       : $($file.VersionInfo.ProductVersion)",
    "TimeDateStamp        : 0x$('{0:X8}' -f $layout.TimeDateStamp) ($(Convert-TimeDateStamp -TimeDateStamp $layout.TimeDateStamp))",
    "TargetedCallMapPath  : $resolvedTargetedCallMapPath",
    "AvailableSeedHelpers : $($seedResult.AllSeeds.Count)",
    "SelectedHelpers      : $($helperFunctions.Count)",
    "CallSitesCaptured    : $($helperCallSites.Count)"
)

$seedLines = @(
    $seedResult.AllSeeds | Select-Object -First ([Math]::Min(10, $seedResult.AllSeeds.Count)) | ForEach-Object {
        $sources = if ($_.SourceSamples.Count -gt 0) { [string]::Join(', ', $_.SourceSamples) } else { '<none>' }
        "Helper=0x$('{0:X8}' -f $_.BeginRva) | TotalInboundCalls=$($_.TotalCalls) | UniqueTransferCallers=$($_.CallerSet.Count) | SampleTransferCallers=$sources"
    }
)
Add-Section -Lines $lines -Title 'Top Transfer-Derived Helper Seeds' -Body $seedLines

foreach ($helper in $helperFunctions) {
    $helperSites = @($helperCallSites | Where-Object { $_.SourceBeginRva -eq $helper.BeginRva })
    $internalSites = @($helperSites | Where-Object { $_.InstructionKind -eq 'CALL_REL' })
    $iatSites = @($helperSites | Where-Object { $_.InstructionKind -ne 'CALL_REL' })
    $helperEdges = @($internalSites | Where-Object { $_.TargetHelper })
    $callerSamples = if ($helper.CallerSamples.Count -gt 0) { [string]::Join(', ', $helper.CallerSamples) } else { '<none>' }

    Add-Section -Lines $lines -Title ("Helper 0x{0:X8}-0x{1:X8}" -f $helper.BeginRva, $helper.EndRva) -Body @(
        "Section             : $($helper.Section)",
        "UnwindRva           : 0x$('{0:X8}' -f $helper.UnwindRva)",
        "TotalInboundCalls   : $($helper.TotalInbound)",
        "UniqueTransferCallers: $($helper.UniqueCallers)",
        "SampleTransferCallers: $callerSamples",
        "DirectInternalCalls : $($internalSites.Count)",
        "DirectIatCalls      : $($iatSites.Count)",
        "HelperToHelperEdges : $($helperEdges.Count)"
    )

    $internalLines = if ($internalSites.Count -gt 0) {
        $internalSites |
        Group-Object TargetLabel |
        Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
        Select-Object -First 20 |
        ForEach-Object {
            $first = $_.Group | Select-Object -First 1
            $helperLabel = if ($first.TargetHelper) { $first.TargetHelper } else { '<none>' }
            "Target=$($_.Name) | Calls=$($_.Count) | TargetSection=$($first.TargetSection) | TargetHelper=$helperLabel | SampleSite=0x$('{0:X8}' -f $first.InstructionRva) | Bytes=$($first.InstructionHex)"
        }
    } else {
        @('No direct relative internal calls captured.')
    }
    Add-Section -Lines $lines -Title ("Internal Calls Helper 0x{0:X8}" -f $helper.BeginRva) -Body $internalLines

    $iatLines = if ($iatSites.Count -gt 0) {
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
    Add-Section -Lines $lines -Title ("IAT Calls Helper 0x{0:X8}" -f $helper.BeginRva) -Body $iatLines
}

$helperEdgeLines = if ($helperFunctions.Count -gt 0) {
    @(
        $helperCallSites |
        Where-Object { $_.TargetHelper } |
        Group-Object { '{0}->{1}' -f ('0x{0:X8}' -f $_.SourceBeginRva), $_.TargetHelper } |
        Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
        ForEach-Object { "$($_.Name) | Calls=$($_.Count)" }
    )
} else {
    @()
}

if ($helperEdgeLines.Count -eq 0) {
    $helperEdgeLines = @('No helper-to-helper internal edges captured.')
}
Add-Section -Lines $lines -Title 'Helper-To-Helper Edges' -Body $helperEdgeLines

Add-Section -Lines $lines -Title 'Notes' -Body @(
    'This pass is read-only and helper-centric. It derives helper seeds from the transfer sections of the targeted call map, then maps only those helper functions.',
    'Internal edges are direct E8 relative calls only. Imported edges are direct RIP-relative CALL/JMP IAT sites only.',
    'The output is intended to show whether the hot transfer clusters converge into a shared helper tier before any deeper host-stack reverse engineering.'
)

Set-Content -Path $resolvedOutputPath -Value $lines -Encoding ASCII
Write-Host "USBXHCI helper call map written to $resolvedOutputPath"
