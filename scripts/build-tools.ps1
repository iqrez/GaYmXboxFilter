[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolsRoot = Join-Path $repoRoot 'GaYmTestFeeder'
$stageRoot = if ($Configuration -eq 'Debug') { Join-Path $repoRoot 'out\dev' } else { Join-Path $repoRoot 'out\release' }
$outRoot = Join-Path $stageRoot 'tools'
$objRoot = Join-Path $outRoot 'obj'

function Find-VcVarsAll {
    $vsWhere = Join-Path "${env:ProgramFiles(x86)}" 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vsWhere) {
        $installationPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($installationPath)) {
            $candidate = Join-Path $installationPath 'VC\Auxiliary\Build\vcvarsall.bat'
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    )

    return $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

$vcvarsAll = Find-VcVarsAll
if (-not $vcvarsAll) {
    throw 'vcvarsall.bat was not found. Install Visual Studio 2022 C++ build tools.'
}

New-Item -ItemType Directory -Force -Path $outRoot, $objRoot | Out-Null
Get-ChildItem -Path $outRoot -File -Filter *.exe -ErrorAction SilentlyContinue | Remove-Item -Force

function Invoke-ToolBuild {
    param(
        [Parameter(Mandatory = $true)]
        [string]$OutputName,
        [Parameter(Mandatory = $true)]
        [string[]]$Sources,
        [string[]]$Libraries = @(),
        [string[]]$CompileFlags = @(),
        [string[]]$LinkFlags = @()
    )

    $quotedSources = ($Sources | ForEach-Object { '"' + (Join-Path $toolsRoot $_) + '"' }) -join ' '
    $includeFlags = @('/I"' + $toolsRoot + '"', '/I"' + $repoRoot + '"') -join ' '
    $libraryFlags = if ($Libraries.Count -gt 0) { $Libraries -join ' ' } else { '' }
    $compileFlags = if ($CompileFlags.Count -gt 0) { $CompileFlags -join ' ' } else { '' }
    $linkFlags = if ($LinkFlags.Count -gt 0) { $LinkFlags -join ' ' } else { '' }
    $outputPath = Join-Path $outRoot $OutputName
    $toolObjRoot = Join-Path $objRoot ($OutputName -replace '\.exe$', '')
    $pdbPath = Join-Path $toolObjRoot ($OutputName -replace '\.exe$', '.pdb')

    New-Item -ItemType Directory -Force -Path $toolObjRoot | Out-Null

    $command = @(
        "call ""$vcvarsAll"" x64 >nul 2>&1",
        "cl.exe /nologo /EHsc /std:c++17 /W4 $includeFlags $compileFlags /Fo:""$toolObjRoot\\"" /Fd:""$pdbPath"" $quotedSources /Fe:""$outputPath"" /link $libraryFlags $linkFlags"
    ) -join ' && '

    Write-Host "=== Building $OutputName ==="
    cmd.exe /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "$OutputName build failed."
    }
}

$builds = @(
    @{ Output = 'GaYmCLI.exe'; Sources = @('GaYmCLI.cpp', 'GuidDefinitions.cpp'); Libraries = @('setupapi.lib') }
    @{ Output = 'GaYmFeeder.exe'; Sources = @('main.cpp', 'GuidDefinitions.cpp', 'Config.cpp', 'KeyboardProvider.cpp', 'MouseProvider.cpp', 'NetworkProvider.cpp', 'MacroProvider.cpp'); Libraries = @('setupapi.lib', 'ws2_32.lib', 'user32.lib') }
    @{ Output = 'AutoVerify.exe'; Sources = @('AutoVerify.cpp', 'GuidDefinitions.cpp'); Libraries = @('setupapi.lib', 'xinput.lib', 'hid.lib') }
    @{ Output = 'FeederAutoVerify.exe'; Sources = @('FeederAutoVerify.cpp', 'GuidDefinitions.cpp'); Libraries = @('setupapi.lib', 'xinput.lib') }
    @{ Output = 'KeyboardFeederAutoVerify.exe'; Sources = @('KeyboardFeederAutoVerify.cpp', 'GuidDefinitions.cpp'); Libraries = @('setupapi.lib', 'xinput.lib', 'user32.lib') }
    @{ Output = 'JoyAutoVerify.exe'; Sources = @('JoyAutoVerify.cpp', 'GuidDefinitions.cpp'); Libraries = @('setupapi.lib', 'winmm.lib') }
    @{ Output = 'HybridAutoVerify.exe'; Sources = @('HybridAutoVerify.cpp', 'GuidDefinitions.cpp'); Libraries = @('setupapi.lib', 'winmm.lib', 'xinput.lib') }
    @{ Output = 'DirectInputAutoVerify.exe'; Sources = @('DirectInputAutoVerify.cpp', 'GuidDefinitions.cpp'); Libraries = @('setupapi.lib', 'dinput8.lib', 'dxguid.lib', 'ole32.lib', 'user32.lib') }
    @{ Output = 'JoySniffer.exe'; Sources = @('JoySniffer.cpp'); Libraries = @('winmm.lib') }
    @{ Output = 'WinMmPacketSniffer.exe'; Sources = @('WinMmPacketSniffer.cpp', 'GuidDefinitions.cpp'); Libraries = @('setupapi.lib', 'winmm.lib') }
    @{ Output = 'DirectInputSniffer.exe'; Sources = @('DirectInputSniffer.cpp'); Libraries = @('dinput8.lib', 'dxguid.lib', 'ole32.lib', 'user32.lib') }
    @{ Output = 'TraceSniffer.exe'; Sources = @('TraceSniffer.cpp', 'GuidDefinitions.cpp'); Libraries = @('setupapi.lib') }
    @{ Output = 'SemanticSniffer.exe'; Sources = @('SemanticSniffer.cpp', 'GuidDefinitions.cpp'); Libraries = @('setupapi.lib') }
    @{ Output = 'RawHidSniffer.exe'; Sources = @('RawHidSniffer.cpp'); Libraries = @('setupapi.lib', 'hid.lib') }
    @{ Output = 'XInputMonitor.exe'; Sources = @('XInputMonitor.cpp'); Libraries = @('user32.lib', 'gdi32.lib', 'xinput.lib'); CompileFlags = @('/DWIN32_LEAN_AND_MEAN'); LinkFlags = @('/SUBSYSTEM:WINDOWS') }
)

foreach ($build in $builds) {
    $compileFlags = @()
    $linkFlags = @()

    if ($build.ContainsKey('CompileFlags')) {
        $compileFlags = $build.CompileFlags
    }

    if ($build.ContainsKey('LinkFlags')) {
        $linkFlags = $build.LinkFlags
    }

    Invoke-ToolBuild -OutputName $build.Output -Sources $build.Sources -Libraries $build.Libraries -CompileFlags $compileFlags -LinkFlags $linkFlags
}

Write-Host ''
Write-Host "Staged tools: $outRoot"
