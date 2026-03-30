[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('Kernel', 'Live')]
    [string]$Source = 'Kernel',
    [ValidateRange(1, 64)]
    [int]$SampleCount = 8,
    [ValidateRange(1, 250)]
    [int]$DueTimeMs = 8,
    [string]$OutputPrefix
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common-paths.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$layout = Get-GaYmArtifactLayout -Root $repoRoot -Configuration $Configuration

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run run-usbxhci-1b1f0-observation.ps1 from an elevated PowerShell session.'
    }
}

function Assert-ToolPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        throw "Required tool not found: $Path"
    }
}

function Invoke-RequiredCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Label,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action
    )

    Write-Host ''
    Write-Host "=== $Label ==="
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed."
    }
}

function New-ObservationArtifactPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LeafName
    )

    return Join-Path $layout.ArtifactRoot $LeafName
}

function Write-ObservationSessionSummary {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$SourceName,
        [Parameter(Mandatory = $true)]
        [int]$RequestedSampleCount,
        [Parameter(Mandatory = $true)]
        [int]$RequestedDueTimeMs,
        [Parameter(Mandatory = $true)]
        [string]$EventsPath,
        [Parameter(Mandatory = $true)]
        [string]$CadencePath,
        [Parameter(Mandatory = $true)]
        [string]$CorrelatedPath,
        [Parameter(Mandatory = $true)]
        [string]$RollupPath,
        [Parameter(Mandatory = $true)]
        [string]$DiagnosticsPath
    )

    $rollupText = Get-Content -Path $RollupPath -Raw

    $lines = @(
        '# USBXHCI 1B1F0 Observation Session'
        ("Captured: {0}" -f (Get-Date).ToString('o'))
        ''
        'TargetBody=0x0001B1F0'
        'PrimaryWindow=timer-lifecycle-and-side-context'
        'SecondaryWindow=time-sampling-and-debug-gate'
        ('Source={0}' -f $SourceName)
        'TransportContract=GAYM_OBSERVATION_EVENT_RECORD'
        ('RequestedSampleCount={0}' -f $RequestedSampleCount)
        ('RequestedDueTimeMs={0}' -f $RequestedDueTimeMs)
        ('EventsPath={0}' -f $EventsPath)
        ('CadencePath={0}' -f $CadencePath)
        ('CorrelatedPath={0}' -f $CorrelatedPath)
        ('RollupPath={0}' -f $RollupPath)
        ('DiagnosticsPath={0}' -f $DiagnosticsPath)
        ''
        '[rollup]'
        $rollupText.TrimEnd()
    )

    $sessionDirectory = Split-Path -Parent $Path
    if (-not (Test-Path $sessionDirectory)) {
        New-Item -ItemType Directory -Path $sessionDirectory -Force | Out-Null
    }

    $lines -join [Environment]::NewLine | Set-Content -Path $Path -Encoding ASCII
}

if (-not $OutputPrefix) {
    $OutputPrefix = 'usbxhci-1b1f0-observation'
}

if ($Source -eq 'Kernel') {
    Assert-Administrator
}

$captureToolName = if ($Source -eq 'Kernel') {
    'ObservationCaptureKernel.exe'
} else {
    'ObservationCaptureLive.exe'
}

$captureTool = Get-GaYmToolPath -Layout $layout -Name $captureToolName
$rollupTool = Get-GaYmToolPath -Layout $layout -Name 'ObservationRollup.exe'

Assert-ToolPath -Path $captureTool
Assert-ToolPath -Path $rollupTool

$eventsPath = New-ObservationArtifactPath -LeafName ("{0}-{1}.bin" -f $OutputPrefix.ToLowerInvariant(), $Source.ToLowerInvariant())
$cadencePath = New-ObservationArtifactPath -LeafName ("{0}-{1}-cadence.csv" -f $OutputPrefix.ToLowerInvariant(), $Source.ToLowerInvariant())
$correlatedPath = New-ObservationArtifactPath -LeafName ("{0}-{1}-correlated.txt" -f $OutputPrefix.ToLowerInvariant(), $Source.ToLowerInvariant())
$rollupPath = New-ObservationArtifactPath -LeafName ("{0}-{1}-rollup.txt" -f $OutputPrefix.ToLowerInvariant(), $Source.ToLowerInvariant())
$diagnosticsPath = New-ObservationArtifactPath -LeafName ("{0}-{1}-diagnostics.txt" -f $OutputPrefix.ToLowerInvariant(), $Source.ToLowerInvariant())
$sessionPath = New-ObservationArtifactPath -LeafName ("{0}-{1}-session.txt" -f $OutputPrefix.ToLowerInvariant(), $Source.ToLowerInvariant())

Invoke-RequiredCommand -Label 'Capture' -Action {
    & $captureTool $eventsPath $SampleCount $DueTimeMs $cadencePath
}

Invoke-RequiredCommand -Label 'Rollup' -Action {
    & $rollupTool $eventsPath $correlatedPath $rollupPath $diagnosticsPath $cadencePath
}

Write-ObservationSessionSummary `
    -Path $sessionPath `
    -SourceName $Source `
    -RequestedSampleCount $SampleCount `
    -RequestedDueTimeMs $DueTimeMs `
    -EventsPath $eventsPath `
    -CadencePath $cadencePath `
    -CorrelatedPath $correlatedPath `
    -RollupPath $rollupPath `
    -DiagnosticsPath $diagnosticsPath

Write-Host ''
Write-Host "Session summary : $sessionPath"
Write-Host "Rollup output   : $rollupPath"
