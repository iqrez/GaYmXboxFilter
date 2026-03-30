[CmdletBinding()]
param(
    [string]$ImagePath,
    [string]$ClusterProfilePath,
    [string]$FeatureMapPath,
    [uint32]$TargetRva = [uint32]0x00008454,
    [uint32[]]$FollowTargetRvas = @(
        [uint32]0x00058B00
    ),
    [int]$NeighborRadius = 2,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$delegateScript = Join-Path $PSScriptRoot 'capture-usbxhci-single-target-deep.ps1'

if (-not (Test-Path -LiteralPath $delegateScript)) {
    throw "Delegate script not found: $delegateScript"
}

if (-not $OutputPath) {
    $outputDirectory = Join-Path $repoRoot 'out\dev'
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    $OutputPath = Join-Path $outputDirectory 'usbxhci-endpoint-target-deep.txt'
}

& $delegateScript `
    -ImagePath $ImagePath `
    -ClusterProfilePath $ClusterProfilePath `
    -FeatureMapPath $FeatureMapPath `
    -TargetRva $TargetRva `
    -FollowTargetRvas $FollowTargetRvas `
    -NeighborRadius $NeighborRadius `
    -OutputPath $OutputPath
