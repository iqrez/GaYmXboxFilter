[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

function Find-MsBuild {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
    )

    $candidate = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($candidate) {
        return $candidate
    }

    $vsWhere = Join-Path "${env:ProgramFiles(x86)}" 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vsWhere) {
        $installationPath = & $vsWhere -latest -version '[17.0,18.0)' -products * -requires Microsoft.Component.MSBuild -property installationPath 2>$null
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($installationPath)) {
            foreach ($fallback in @(
                (Join-Path $installationPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'),
                (Join-Path $installationPath 'MSBuild\Current\Bin\MSBuild.exe')
            )) {
                if (Test-Path $fallback) {
                    return $fallback
                }
            }
        }
    }

    return $null
}

$msbuild = Find-MsBuild
if (-not $msbuild) {
    throw 'MSBuild.exe was not found. Install Visual Studio 2022 Build Tools with WDK integration.'
}

if ($Configuration -eq 'Debug') {
    $stageRoot = Join-Path $repoRoot 'out\dev'
} else {
    $stageRoot = Join-Path $repoRoot 'out\release'
}

$upperStage = Join-Path $stageRoot 'driver-upper'
$lowerStage = Join-Path $stageRoot 'driver-lower'

foreach ($path in @($upperStage, $lowerStage)) {
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    Get-ChildItem -Path $path -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

$projects = @(
    @{
        Name = 'GaYmXboxFilter'
        Project = Join-Path $repoRoot 'GaYmXboxFilter.vcxproj'
        StageDir = $upperStage
        Files = @(
            @{ Source = Join-Path $repoRoot "x64\$Configuration\GaYmXboxFilter.sys"; Target = 'GaYmXboxFilter.sys' }
            @{ Source = Join-Path $repoRoot "x64\$Configuration\GaYmXboxFilter.inf"; Target = 'GaYmXboxFilter.inf' }
            @{ Source = Join-Path $repoRoot "x64\$Configuration\GaYmXboxFilter\GaYmXboxFilter.inf"; Target = 'package\GaYmXboxFilter.inf' }
            @{ Source = Join-Path $repoRoot "x64\$Configuration\GaYmXboxFilter\GaYmXboxFilter.sys"; Target = 'package\GaYmXboxFilter.sys' }
            @{ Source = Join-Path $repoRoot "x64\$Configuration\GaYmXboxFilter\gaymxboxfilter.cat"; Target = 'package\gaymxboxfilter.cat' }
        )
    }
    @{
        Name = 'GaYmFilter'
        Project = Join-Path $repoRoot 'GaYmFilter.vcxproj'
        StageDir = $lowerStage
        Files = @(
            @{ Source = Join-Path $repoRoot "x64\$Configuration\GaYmFilter\GaYmFilter.sys"; Target = 'GaYmFilter.sys' }
            @{ Source = Join-Path $repoRoot "x64\$Configuration\GaYmFilter\GaYmFilter.inf"; Target = 'GaYmFilter.inf' }
            @{ Source = Join-Path $repoRoot "x64\$Configuration\GaYmFilter\GaYmFilter\GaYmFilter.inf"; Target = 'package\GaYmFilter.inf' }
            @{ Source = Join-Path $repoRoot "x64\$Configuration\GaYmFilter\GaYmFilter\GaYmFilter.sys"; Target = 'package\GaYmFilter.sys' }
            @{ Source = Join-Path $repoRoot "x64\$Configuration\GaYmFilter\GaYmFilter\gaymfilter.cat"; Target = 'package\gaymfilter.cat' }
        )
    }
)

foreach ($project in $projects) {
    Write-Host "=== Building $($project.Name) ($Platform $Configuration) ==="
    & $msbuild $project.Project "/p:Configuration=$Configuration" "/p:Platform=$Platform" '/t:Rebuild'
    if ($LASTEXITCODE -ne 0) {
        throw "$($project.Name) build failed."
    }

    foreach ($file in $project.Files) {
        if (-not (Test-Path $file.Source)) {
            throw "Expected build output not found: $($file.Source)"
        }

        $targetPath = Join-Path $project.StageDir $file.Target
        $targetDir = Split-Path -Parent $targetPath
        New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
        Copy-Item -Force -Path $file.Source -Destination $targetPath
    }
}

Write-Host ''
Write-Host "Staged upper package: $upperStage"
Write-Host "Staged lower package: $lowerStage"
