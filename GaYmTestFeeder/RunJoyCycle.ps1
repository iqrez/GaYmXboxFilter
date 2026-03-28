param(
    [int]$HoldMs = 1200,
    [int]$NeutralMs = 600,
    [string]$LogPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-Log {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Message,
        [ConsoleColor]$Color = [ConsoleColor]::Gray
    )

    $timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'
    $line = "[${timestamp}] $Message"
    Add-Content -LiteralPath $script:LogPath -Value $line -Encoding ascii
    Write-Host $line -ForegroundColor $Color
}

function Invoke-LoggedNativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)]
        [string]$Label,
        [switch]$AllowFailure
    )

    $quotedArguments = ($Arguments | ForEach-Object {
            if ($_ -match '\s') { '"{0}"' -f $_ } else { $_ }
        }) -join ' '
    $commandText = if ([string]::IsNullOrWhiteSpace($quotedArguments)) {
        $FilePath
    } else {
        "$FilePath $quotedArguments"
    }

    Write-Log "BEGIN $Label :: $commandText" ([ConsoleColor]::Cyan)
    $output = @(& $FilePath @Arguments 2>&1)
    $exitCode = $LASTEXITCODE

    foreach ($line in $output) {
        if ($null -eq $line) {
            continue
        }

        $text = [string]$line
        Add-Content -LiteralPath $script:LogPath -Value "    $text" -Encoding ascii
        Write-Host $text
    }

    if ($null -eq $exitCode) {
        $exitCode = 0
    }

    $statusColor = if ($exitCode -eq 0) { [ConsoleColor]::Green } else { [ConsoleColor]::Yellow }
    Write-Log "END   $Label :: exit=$exitCode" $statusColor
    if (-not $AllowFailure -and $exitCode -ne 0) {
        throw "$Label failed with exit code $exitCode"
    }

    return @{
        ExitCode = $exitCode
        Output = $output
    }
}

function New-MacroEntry {
    param(
        [int]$DelayMs,
        [byte]$Buttons0 = 0,
        [byte]$Buttons1 = 0,
        [byte]$DPad = 15,
        [byte]$LT = 0,
        [byte]$RT = 0,
        [int16]$LX = 0,
        [int16]$LY = 0,
        [int16]$RX = 0,
        [int16]$RY = 0
    )

    '{0},{1},{2},{3},{4},{5},{6},{7},{8},{9}' -f `
        $DelayMs, $Buttons0, $Buttons1, $DPad, $LT, $RT, $LX, $LY, $RX, $RY
}

function Add-Step {
    param(
        [System.Collections.Generic.List[string]]$MacroLines,
        [ref]$CursorMs,
        [string]$Label,
        [byte]$Buttons0 = 0,
        [byte]$Buttons1 = 0,
        [byte]$DPad = 15,
        [byte]$LT = 0,
        [byte]$RT = 0,
        [int16]$LX = 0,
        [int16]$LY = 0,
        [int16]$RX = 0,
        [int16]$RY = 0
    )

    Write-Log ("[{0,6} ms] {1}" -f $CursorMs.Value, $Label)
    $MacroLines.Add((New-MacroEntry -DelayMs $CursorMs.Value -Buttons0 $Buttons0 -Buttons1 $Buttons1 -DPad $DPad -LT $LT -RT $RT -LX $LX -LY $LY -RX $RX -RY $RY))
    $CursorMs.Value += $HoldMs
    $MacroLines.Add((New-MacroEntry -DelayMs $CursorMs.Value))
    $CursorMs.Value += $NeutralMs
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptRoot

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $scriptRoot ("RunJoyCycle_{0:yyyyMMdd_HHmmss}.log" -f (Get-Date))
}

$logDirectory = Split-Path -Parent $LogPath
if (-not [string]::IsNullOrWhiteSpace($logDirectory)) {
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
}
Set-Content -LiteralPath $LogPath -Value @(
    "# RunJoyCycle log"
    "# HoldMs=$HoldMs NeutralMs=$NeutralMs"
    "# Started=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')"
) -Encoding ascii

Write-Log "LogPath: $LogPath" ([ConsoleColor]::Cyan)
Write-Log "ScriptRoot: $scriptRoot"

$cliPath = Join-Path $scriptRoot 'GaYmCLI.exe'
$feederPath = Join-Path $scriptRoot 'GaYmFeeder.exe'
if (-not (Test-Path $cliPath)) {
    throw "Missing $cliPath"
}
if (-not (Test-Path $feederPath)) {
    throw "Missing $feederPath"
}

Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('off', '0') -Label 'Initial override off' -AllowFailure | Out-Null

$macroLines = [System.Collections.Generic.List[string]]::new()
$cursorMs = 0
$macroLines.Add((New-MacroEntry -DelayMs $cursorMs))
$cursorMs += 250

Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'A'     -Buttons0 0x01
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'B'     -Buttons0 0x02
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'X'     -Buttons0 0x04
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Y'     -Buttons0 0x08
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'LB'    -Buttons0 0x10
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'RB'    -Buttons0 0x20
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Back'  -Buttons0 0x40
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Start' -Buttons0 0x80

Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'L3'    -Buttons1 0x01
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'R3'    -Buttons1 0x02
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Guide' -Buttons1 0x04
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Share' -Buttons1 0x08

Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'DPad Up'    -DPad 0
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'DPad Right' -DPad 2
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'DPad Down'  -DPad 4
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'DPad Left'  -DPad 6

Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Left Trigger'  -LT 255
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Right Trigger' -RT 255

Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Left Stick Right' -LX 32767
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Left Stick Left'  -LX -32767
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Left Stick Up'    -LY -32767
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Left Stick Down'  -LY 32767

Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Right Stick Right' -RX 32767
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Right Stick Left'  -RX -32767
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Right Stick Up'    -RY -32767
Add-Step -MacroLines $macroLines -CursorMs ([ref]$cursorMs) -Label 'Right Stick Down'  -RY 32767

$durationMs = $cursorMs + 1000
$macroPath = Join-Path $env:TEMP 'GaYmJoyCycle.csv'
$configPath = Join-Path $env:TEMP 'GaYmJoyCycle.ini'

$config = @"
[General]
Provider = macro
PollRateHz = 125
DeviceIndex = 0

[Jitter]
Enabled = false
MinUs = 0
MaxUs = 0

[Network]
BindAddr = 127.0.0.1
Port = 43210

[Macros]
File = $macroPath
Loop = false
"@

Set-Content -LiteralPath $macroPath -Value ($macroLines -join [Environment]::NewLine) -Encoding ascii
Set-Content -LiteralPath $configPath -Value $config -Encoding ascii
Write-Log "Macro CSV written: $macroPath"
Write-Log "Config INI written: $configPath"
Write-Log "Macro steps: $($macroLines.Count)"
Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('status') -Label 'Pre-run status' -AllowFailure | Out-Null

Write-Log ""
Write-Log "Launching GaYmFeeder macro cycle..." ([ConsoleColor]::Cyan)
Write-Log "Hold: $HoldMs ms | Neutral gap: $NeutralMs ms | Total: $durationMs ms"
Write-Log ""

try {
    Invoke-LoggedNativeCommand -FilePath $feederPath -Arguments @('-p', 'macro', '-c', $configPath, '--duration-ms', "$durationMs") -Label 'GaYmFeeder macro cycle' | Out-Null
}
finally {
    Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('off', '0') -Label 'Final override off' -AllowFailure | Out-Null
    Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('status') -Label 'Post-run status' -AllowFailure | Out-Null
    Remove-Item -LiteralPath $macroPath, $configPath -ErrorAction SilentlyContinue
    Write-Log "Removed temporary files."
    Write-Log ""
    Write-Log "Cycle complete. Override disabled." ([ConsoleColor]::Green)
}
