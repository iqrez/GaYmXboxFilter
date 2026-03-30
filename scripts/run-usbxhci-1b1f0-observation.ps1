[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('Kernel', 'Live', 'HostEmitter')]
    [string]$Source = 'Kernel',
    [ValidateRange(1, 64)]
    [int]$SampleCount = 8,
    [ValidateRange(1, 250)]
    [int]$DueTimeMs = 8,
    [string]$OutputPrefix,
    [string]$CaptureToolPath,
    [ValidateSet('Adapter', 'Import')]
    [string]$HostEmitterBackend = 'Adapter',
    [string]$HostEmitterImportEvents,
    [string]$HostEmitterImportCadence
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

function Resolve-ObservationSource {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceName,
        [Parameter(Mandatory = $true)]
        [pscustomobject]$ResolvedLayout,
        [string]$ExplicitToolPath,
        [string]$HostBackendName
    )

    switch ($SourceName) {
        'Kernel' {
            return [pscustomobject]@{
                Name          = 'Kernel'
                Role          = 'LowerFilterControlPath'
                RequiresAdmin = $true
                ToolPath      = Get-GaYmToolPath -Layout $ResolvedLayout -Name 'ObservationCaptureKernel.exe'
                Resolution    = 'RepoLocalTool'
            }
        }

        'Live' {
            return [pscustomobject]@{
                Name          = 'Live'
                Role          = 'InProcessWaitableTimer'
                RequiresAdmin = $false
                ToolPath      = Get-GaYmToolPath -Layout $ResolvedLayout -Name 'ObservationCaptureLive.exe'
                Resolution    = 'RepoLocalTool'
            }
        }

        'HostEmitter' {
            if ($ExplicitToolPath) {
                return [pscustomobject]@{
                    Name          = 'HostEmitter'
                    Role          = 'FutureUsbXhciEmitter'
                    Backend       = $HostBackendName
                    RequiresAdmin = ($HostBackendName -eq 'Adapter')
                    ToolPath      = $ExplicitToolPath
                    Resolution    = 'ExplicitToolPath'
                }
            }

            return [pscustomobject]@{
                Name          = 'HostEmitter'
                Role          = 'FutureUsbXhciEmitter'
                Backend       = $HostBackendName
                RequiresAdmin = ($HostBackendName -eq 'Adapter')
                ToolPath      = Get-GaYmToolPath -Layout $ResolvedLayout -Name 'ObservationCaptureUsbXhciHost.exe'
                Resolution    = 'DefaultFutureTool'
            }
        }
    }

    throw "Unsupported source: $SourceName"
}

function Assert-OptionalPathExists {
    param(
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    if ($Path -and -not (Test-Path $Path)) {
        throw "$Label not found: $Path"
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
        [pscustomobject]$SourceInfo,
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
    $sourceBackend = 'n/a'
    if ($null -ne $SourceInfo.PSObject.Properties['Backend'] -and $SourceInfo.Backend) {
        $sourceBackend = $SourceInfo.Backend
    }

    $lines = @(
        '# USBXHCI 1B1F0 Observation Session'
        ("Captured: {0}" -f (Get-Date).ToString('o'))
        ''
        'TargetBody=0x0001B1F0'
        'PrimaryWindow=timer-lifecycle-and-side-context'
        'SecondaryWindow=time-sampling-and-debug-gate'
        ('Source={0}' -f $SourceInfo.Name)
        ('SourceRole={0}' -f $SourceInfo.Role)
        ('SourceResolution={0}' -f $SourceInfo.Resolution)
        ('SourceBackend={0}' -f $sourceBackend)
        ('CaptureTool={0}' -f $SourceInfo.ToolPath)
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

function Invoke-ObservationCapture {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$SourceInfo,
        [Parameter(Mandatory = $true)]
        [string]$CaptureTool,
        [Parameter(Mandatory = $true)]
        [string]$EventsPath,
        [Parameter(Mandatory = $true)]
        [int]$RequestedSampleCount,
        [Parameter(Mandatory = $true)]
        [int]$RequestedDueTimeMs,
        [Parameter(Mandatory = $true)]
        [string]$CadencePath,
        [string]$ImportEventsPath,
        [string]$ImportCadencePath
    )

    $previousBackend = $null
    $previousImportEvents = $null
    $previousImportCadence = $null

    if ($SourceInfo.Name -eq 'HostEmitter') {
        $previousBackend = $env:GAYM_HOST_EMITTER_BACKEND
        $previousImportEvents = $env:GAYM_HOST_EMITTER_IMPORT_EVENTS
        $previousImportCadence = $env:GAYM_HOST_EMITTER_IMPORT_CADENCE

        $env:GAYM_HOST_EMITTER_BACKEND = $SourceInfo.Backend.ToLowerInvariant()
        if ($ImportEventsPath) {
            $env:GAYM_HOST_EMITTER_IMPORT_EVENTS = $ImportEventsPath
        } else {
            Remove-Item Env:GAYM_HOST_EMITTER_IMPORT_EVENTS -ErrorAction SilentlyContinue
        }

        if ($ImportCadencePath) {
            $env:GAYM_HOST_EMITTER_IMPORT_CADENCE = $ImportCadencePath
        } else {
            Remove-Item Env:GAYM_HOST_EMITTER_IMPORT_CADENCE -ErrorAction SilentlyContinue
        }
    }

    try {
        & $CaptureTool $EventsPath $RequestedSampleCount $RequestedDueTimeMs $CadencePath
        if ($LASTEXITCODE -ne 0) {
            throw 'Capture failed.'
        }
    } finally {
        if ($SourceInfo.Name -eq 'HostEmitter') {
            if ($null -ne $previousBackend) {
                $env:GAYM_HOST_EMITTER_BACKEND = $previousBackend
            } else {
                Remove-Item Env:GAYM_HOST_EMITTER_BACKEND -ErrorAction SilentlyContinue
            }

            if ($null -ne $previousImportEvents) {
                $env:GAYM_HOST_EMITTER_IMPORT_EVENTS = $previousImportEvents
            } else {
                Remove-Item Env:GAYM_HOST_EMITTER_IMPORT_EVENTS -ErrorAction SilentlyContinue
            }

            if ($null -ne $previousImportCadence) {
                $env:GAYM_HOST_EMITTER_IMPORT_CADENCE = $previousImportCadence
            } else {
                Remove-Item Env:GAYM_HOST_EMITTER_IMPORT_CADENCE -ErrorAction SilentlyContinue
            }
        }
    }
}

if (-not $OutputPrefix) {
    $OutputPrefix = 'usbxhci-1b1f0-observation'
}

if ($Source -eq 'HostEmitter') {
    Assert-OptionalPathExists -Path $HostEmitterImportEvents -Label 'HostEmitter import events'
    Assert-OptionalPathExists -Path $HostEmitterImportCadence -Label 'HostEmitter import cadence'
    if ($HostEmitterBackend -eq 'Import' -and -not $HostEmitterImportEvents) {
        throw 'HostEmitter import backend requires -HostEmitterImportEvents.'
    }
}

$sourceInfo = Resolve-ObservationSource `
    -SourceName $Source `
    -ResolvedLayout $layout `
    -ExplicitToolPath $CaptureToolPath `
    -HostBackendName $HostEmitterBackend

if ($sourceInfo.RequiresAdmin) {
    Assert-Administrator
}

$captureTool = $sourceInfo.ToolPath
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
    Invoke-ObservationCapture `
        -SourceInfo $sourceInfo `
        -CaptureTool $captureTool `
        -EventsPath $eventsPath `
        -RequestedSampleCount $SampleCount `
        -RequestedDueTimeMs $DueTimeMs `
        -CadencePath $cadencePath `
        -ImportEventsPath $HostEmitterImportEvents `
        -ImportCadencePath $HostEmitterImportCadence
}

Invoke-RequiredCommand -Label 'Rollup' -Action {
    & $rollupTool $eventsPath $correlatedPath $rollupPath $diagnosticsPath $cadencePath
}

Write-ObservationSessionSummary `
    -Path $sessionPath `
    -SourceInfo $sourceInfo `
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
