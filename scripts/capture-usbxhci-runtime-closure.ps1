[CmdletBinding()]
param(
    [string]$ImagePath,
    [string[]]$BaselineWalkPaths,
    [string]$ClosureWalkPath,
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

    return Join-Path $env:SystemRoot 'System32\drivers\USBXHCI.SYS'
}

function Resolve-BaselineWalkPaths {
    param([string[]]$RequestedPaths)

    if ($RequestedPaths -and $RequestedPaths.Count -gt 0) {
        return $RequestedPaths
    }

    $defaultPaths = @(
        (Join-Path (Get-RepoRoot) 'out\dev\usbxhci-exhaustive-walk.txt'),
        (Join-Path (Get-RepoRoot) 'out\dev\usbxhci-feature-closure-walk.txt')
    )

    return @($defaultPaths | Where-Object { Test-Path -LiteralPath $_ })
}

function Resolve-ClosureWalkPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-runtime-closure-walk.txt'
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-runtime-closure.txt'
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
    $sizeOfOptionalHeader = Read-UInt16LE -Bytes $Bytes -Offset ($coffOffset + 16)
    $optionalHeaderOffset = $coffOffset + 20
    $magic = Read-UInt16LE -Bytes $Bytes -Offset $optionalHeaderOffset
    $isPe32Plus = ($magic -eq 0x20B)
    if (-not $isPe32Plus -and $magic -ne 0x10B) {
        throw ('Unsupported optional header magic 0x{0:X4}' -f $magic)
    }

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
        $nameBytes = $Bytes[$sectionOffset..($sectionOffset + 7)]
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
        SizeOfHeaders  = $sizeOfHeaders
        DataDirectories = $dataDirectories
        Sections       = $sections
    }
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
        $span = [Math]::Max([uint32]$section.VirtualSize, [uint32]$section.SizeOfRawData)
        if ($Rva -ge $section.VirtualAddress -and $Rva -lt ($section.VirtualAddress + $span)) {
            return [int]($section.PointerToRawData + ($Rva - $section.VirtualAddress))
        }
    }

    return $null
}

function Parse-RuntimeFunctions {
    param(
        [byte[]]$Bytes,
        [object]$Layout
    )

    $exceptionDirectory = $Layout.DataDirectories | Where-Object { $_.Index -eq 3 } | Select-Object -First 1
    if (-not $exceptionDirectory -or $exceptionDirectory.RVA -eq 0 -or $exceptionDirectory.Size -eq 0) {
        throw 'Exception directory is missing.'
    }

    $directoryOffset = Convert-RvaToOffset -Rva $exceptionDirectory.RVA -Sections $Layout.Sections -SizeOfHeaders $Layout.SizeOfHeaders
    if ($null -eq $directoryOffset) {
        throw 'Could not resolve exception directory RVA.'
    }

    $entryCount = [int]($exceptionDirectory.Size / 12)
    $functions = New-Object 'System.Collections.Generic.List[string]'
    for ($index = 0; $index -lt $entryCount; $index++) {
        $entryOffset = $directoryOffset + ($index * 12)
        $startRva = Read-UInt32LE -Bytes $Bytes -Offset $entryOffset
        $endRva = Read-UInt32LE -Bytes $Bytes -Offset ($entryOffset + 4)
        if ($startRva -eq 0 -or $endRva -le $startRva) {
            continue
        }

        $functions.Add(('0x{0:X8}' -f $startRva)) | Out-Null
    }

    return @($functions | Sort-Object -Unique)
}

function Get-VisitedTargets {
    param([string[]]$Paths)

    $visited = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($path in $Paths) {
        foreach ($line in Get-Content -Path $path) {
            if ($line -match '^## Target (0x[0-9A-F]+)$') {
                [void]$visited.Add($matches[1])
            }
        }
    }

    return (, $visited)
}

function Convert-HexStringToUInt32 {
    param([string]$Value)

    return [uint32]::Parse($Value.Substring(2), [System.Globalization.NumberStyles]::HexNumber)
}

$imagePath = Resolve-ImagePath -RequestedPath $ImagePath
$baselinePaths = Resolve-BaselineWalkPaths -RequestedPaths $BaselineWalkPaths
$closureWalkPath = Resolve-ClosureWalkPath -RequestedPath $ClosureWalkPath
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath
$exhaustiveWalkScript = Join-Path $PSScriptRoot 'capture-usbxhci-exhaustive-walk.ps1'

foreach ($path in @($imagePath, $exhaustiveWalkScript) + $baselinePaths) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required file not found: $path"
    }
}

$outputDirectory = Split-Path -Parent $outputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$bytes = [System.IO.File]::ReadAllBytes($imagePath)
$layout = Parse-PeLayout -Bytes $bytes
$runtimeFunctions = Parse-RuntimeFunctions -Bytes $bytes -Layout $layout
$baselineVisited = Get-VisitedTargets -Paths $baselinePaths
$missingSeeds = @($runtimeFunctions | Where-Object { -not $baselineVisited.Contains($_) })

if ($missingSeeds.Count -gt 0) {
    $seedValues = @($missingSeeds | ForEach-Object { Convert-HexStringToUInt32 -Value $_ })
    & $exhaustiveWalkScript -SeedRvas $seedValues -OutputPath $closureWalkPath *> $null
}

$combinedVisited = Get-VisitedTargets -Paths $baselinePaths
if (Test-Path -LiteralPath $closureWalkPath) {
    $closureVisited = Get-VisitedTargets -Paths @($closureWalkPath)
    foreach ($target in $closureVisited) {
        [void]$combinedVisited.Add($target)
    }
}

$stillMissing = @($runtimeFunctions | Where-Object { -not $combinedVisited.Contains($_) })
$summaryLines = New-Object 'System.Collections.Generic.List[string]'

$summaryLines.Add('# USBXHCI Runtime Closure') | Out-Null
$summaryLines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$summaryLines.Add('') | Out-Null

Add-Section -Lines $summaryLines -Title 'Summary' -Body @(
    "RuntimeFunctionCount  : $($runtimeFunctions.Count)",
    "BaselineWalkCount     : $($baselinePaths.Count)",
    "BaselineVisitedCount  : $($baselineVisited.Count)",
    "MissingSeedCount      : $($missingSeeds.Count)",
    "ClosureWalkPath       : $closureWalkPath",
    "CombinedVisitedCount  : $($combinedVisited.Count)",
    "StillMissingCount     : $($stillMissing.Count)"
)

Add-Section -Lines $summaryLines -Title 'Baseline Walk Paths' -Body @(
    if ($baselinePaths.Count -eq 0) {
        '<none>'
    } else {
        $baselinePaths
    }
)

Add-Section -Lines $summaryLines -Title 'Still Missing Runtime Functions' -Body @(
    if ($stillMissing.Count -eq 0) {
        '<none>'
    } else {
        $stillMissing
    }
)

Set-Content -Path $outputPath -Value $summaryLines -Encoding ASCII
Write-Output "Wrote $outputPath"
