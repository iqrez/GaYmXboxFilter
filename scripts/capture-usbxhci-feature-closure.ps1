[CmdletBinding()]
param(
    [string]$FeatureMapPath,
    [string]$BaselineWalkPath,
    [string]$ClosureWalkPath,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-FeatureMapPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-feature-map.txt'
}

function Resolve-BaselineWalkPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-exhaustive-walk.txt'
}

function Resolve-ClosureWalkPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-feature-closure-walk.txt'
}

function Resolve-OutputPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    return Join-Path (Get-RepoRoot) 'out\dev\usbxhci-feature-closure.txt'
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

function Get-VisitedTargets {
    param([string]$Path)

    $visited = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($line in Get-Content -Path $Path) {
        if ($line -match '^## Target (0x[0-9A-F]+)$') {
            [void]$visited.Add($matches[1])
        }
    }

    return (, $visited)
}

function Get-FeatureCandidates {
    param([string]$Path)

    $candidates = New-Object 'System.Collections.Generic.List[string]'
    foreach ($line in Get-Content -Path $Path) {
        if ($line -match '^Function=(0x[0-9A-F]+)-') {
            $candidates.Add($matches[1]) | Out-Null
        }
    }

    return @($candidates | Sort-Object -Unique)
}

function Convert-HexStringToUInt32 {
    param([string]$Value)

    return [uint32]::Parse($Value.Substring(2), [System.Globalization.NumberStyles]::HexNumber)
}

$featureMapPath = Resolve-FeatureMapPath -RequestedPath $FeatureMapPath
$baselineWalkPath = Resolve-BaselineWalkPath -RequestedPath $BaselineWalkPath
$closureWalkPath = Resolve-ClosureWalkPath -RequestedPath $ClosureWalkPath
$outputPath = Resolve-OutputPath -RequestedPath $OutputPath
$exhaustiveWalkScript = Join-Path $PSScriptRoot 'capture-usbxhci-exhaustive-walk.ps1'

foreach ($path in @($featureMapPath, $baselineWalkPath, $exhaustiveWalkScript)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required file not found: $path"
    }
}

$outputDirectory = Split-Path -Parent $outputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$featureCandidates = Get-FeatureCandidates -Path $featureMapPath
$baselineVisited = Get-VisitedTargets -Path $baselineWalkPath
$missingSeeds = @($featureCandidates | Where-Object { -not $baselineVisited.Contains($_) })

if ($missingSeeds.Count -gt 0) {
    $seedValues = @($missingSeeds | ForEach-Object { Convert-HexStringToUInt32 -Value $_ })
    & $exhaustiveWalkScript -SeedRvas $seedValues -OutputPath $closureWalkPath *> $null
}

$combinedVisited = Get-VisitedTargets -Path $baselineWalkPath
if (Test-Path -LiteralPath $closureWalkPath) {
    $closureVisited = Get-VisitedTargets -Path $closureWalkPath
    foreach ($target in $closureVisited) {
        [void]$combinedVisited.Add($target)
    }
}

$stillMissing = @($featureCandidates | Where-Object { -not $combinedVisited.Contains($_) })
$summaryLines = New-Object 'System.Collections.Generic.List[string]'

$summaryLines.Add('# USBXHCI Feature Closure') | Out-Null
$summaryLines.Add("Captured: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')") | Out-Null
$summaryLines.Add('') | Out-Null

Add-Section -Lines $summaryLines -Title 'Summary' -Body @(
    "FeatureCandidateCount : $($featureCandidates.Count)",
    "BaselineVisitedCount  : $($baselineVisited.Count)",
    "MissingSeedCount      : $($missingSeeds.Count)",
    "ClosureWalkPath       : $closureWalkPath",
    "CombinedVisitedCount  : $($combinedVisited.Count)",
    "StillMissingCount     : $($stillMissing.Count)"
)

Add-Section -Lines $summaryLines -Title 'Missing Seeds' -Body @(
    if ($missingSeeds.Count -eq 0) {
        '<none>'
    } else {
        $missingSeeds
    }
)

Add-Section -Lines $summaryLines -Title 'Still Missing Feature Candidates' -Body @(
    if ($stillMissing.Count -eq 0) {
        '<none>'
    } else {
        $stillMissing
    }
)

Set-Content -Path $outputPath -Value $summaryLines -Encoding ASCII
Write-Output "Wrote $outputPath"
