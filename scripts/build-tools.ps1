[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

function Get-VcVarsAllPath {
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vsWhere)) {
        throw "vswhere.exe not found: $vsWhere"
    }

    $installPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    if (-not $installPath) {
        throw 'Could not locate a Visual Studio installation with C++ tools.'
    }

    $vcVarsAll = Join-Path $installPath 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path -LiteralPath $vcVarsAll)) {
        throw "vcvarsall.bat not found: $vcVarsAll"
    }

    return $vcVarsAll
}

function Invoke-ToolBuild {
    param(
        [string]$VcVarsAll,
        [string]$WorkingDirectory,
        [string]$CommandLine
    )

    $fullCommand = 'call "' + $VcVarsAll + '" x64 >nul && cd /d "' + $WorkingDirectory + '" && ' + $CommandLine
    & cmd.exe /d /c $fullCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Tool build failed: $CommandLine"
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$legacyBatch = Join-Path $repoRoot 'build_usermode.bat'
$clientScript = Join-Path $PSScriptRoot 'build-client.ps1'
$toolRoot = Join-Path $repoRoot 'src\tools\GaYmTestFeeder'
$sharedRoot = Join-Path $repoRoot 'src\shared'
$clientRoot = Join-Path $repoRoot 'src\client'
$clientLib = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev\client\gaym_client.lib'
} else {
    Join-Path $repoRoot 'out\release\client\gaym_client.lib'
}
$profileRoot = if ($Configuration -eq 'Debug') {
    Join-Path $repoRoot 'out\dev\tools'
} else {
    Join-Path $repoRoot 'out\release\tools'
}
$objRoot = Join-Path $profileRoot 'obj'
$vcVarsAll = Get-VcVarsAllPath

if ($Configuration -eq 'Debug') {
    if (-not (Test-Path -LiteralPath $legacyBatch)) {
        throw "Tool batch entrypoint not found: $legacyBatch"
    }

    Write-Host "Building tools via batch fallback: $legacyBatch"
    & cmd.exe /d /c "`"$legacyBatch`""
    if ($LASTEXITCODE -ne 0) {
        throw "Tool batch build failed with exit code $LASTEXITCODE."
    }

    Write-Host ''
    Write-Host "Tool output directory: $profileRoot"
    Get-ChildItem -LiteralPath $profileRoot -Filter *.exe | Select-Object Name, Length
    return
}

if (-not (Test-Path -LiteralPath $toolRoot)) {
    throw "Tool source directory not found: $toolRoot"
}
if (-not (Test-Path -LiteralPath $sharedRoot)) {
    throw "Shared include directory not found: $sharedRoot"
}
if (-not (Test-Path -LiteralPath $clientRoot)) {
    throw "Client source directory not found: $clientRoot"
}

if (-not (Test-Path -LiteralPath $clientLib)) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $clientScript -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "Client build failed with exit code $LASTEXITCODE."
    }
}

New-Item -ItemType Directory -Force -Path $profileRoot | Out-Null
New-Item -ItemType Directory -Force -Path $objRoot | Out-Null

$commonFlags = '/nologo /EHsc /std:c++17 /W4 /I. /I"' + $sharedRoot + '" /I"' + $clientRoot + '" /Fo:"' + $objRoot + '\\\\" /Fd:"' + $objRoot + '\\vc143.pdb"'

$builds = @(
    'cl.exe ' + $commonFlags + ' GaYmCLI.cpp /Fe:"' + (Join-Path $profileRoot 'GaYmCLI.exe') + '" /link "' + $clientLib + '" setupapi.lib',
    'cl.exe ' + $commonFlags + ' main.cpp Config.cpp KeyboardProvider.cpp MouseProvider.cpp NetworkProvider.cpp MacroProvider.cpp /Fe:"' + (Join-Path $profileRoot 'GaYmFeeder.exe') + '" /link "' + $clientLib + '" setupapi.lib ws2_32.lib user32.lib',
    'cl.exe ' + $commonFlags + ' MinimalTestFeeder.cpp KeyboardProvider.cpp /Fe:"' + (Join-Path $profileRoot 'MinimalTestFeeder.exe') + '" /link "' + $clientLib + '" setupapi.lib user32.lib',
    'cl.exe ' + $commonFlags + ' AutoVerify.cpp /Fe:"' + (Join-Path $profileRoot 'AutoVerify.exe') + '" /link "' + $clientLib + '" setupapi.lib xinput.lib hid.lib',
    'cl.exe ' + $commonFlags + ' QuickVerify.cpp /Fe:"' + (Join-Path $profileRoot 'QuickVerify.exe') + '" /link "' + $clientLib + '" xinput.lib setupapi.lib',
    'cl.exe ' + $commonFlags + ' QuickVerifyHid.cpp /Fe:"' + (Join-Path $profileRoot 'QuickVerifyHid.exe') + '" /link "' + $clientLib + '" setupapi.lib hid.lib',
    'cl.exe ' + $commonFlags + ' XInputCheck.cpp /Fe:"' + (Join-Path $profileRoot 'XInputCheck.exe') + '" /link xinput.lib'
)

Write-Host "Using vcvarsall: $vcVarsAll"
Write-Host "Building tools from: $toolRoot"

foreach ($build in $builds) {
    Invoke-ToolBuild -VcVarsAll $vcVarsAll -WorkingDirectory $toolRoot -CommandLine $build
}

Write-Host ''
Write-Host "Tool output directory: $profileRoot"
Get-ChildItem -LiteralPath $profileRoot -Filter *.exe | Select-Object Name, Length
