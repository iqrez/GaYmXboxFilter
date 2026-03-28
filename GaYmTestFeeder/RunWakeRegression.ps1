param(
    [int]$BaselineSeconds = 15,
    [int]$PostWakeSeconds = 60,
    [int]$PollIntervalMs = 500,
    [int]$DeviceIndex = 0,
    [switch]$SkipResumePrompt,
    [switch]$SkipAutoVerify,
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

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptRoot

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $scriptRoot ("WakeRegression_{0:yyyyMMdd_HHmmss}.log" -f (Get-Date))
}

$logDirectory = Split-Path -Parent $LogPath
if (-not [string]::IsNullOrWhiteSpace($logDirectory)) {
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
}

Set-Content -LiteralPath $LogPath -Value @(
    "# WakeRegression log"
    "# BaselineSeconds=$BaselineSeconds PostWakeSeconds=$PostWakeSeconds PollIntervalMs=$PollIntervalMs DeviceIndex=$DeviceIndex"
    "# SkipResumePrompt=$SkipResumePrompt SkipAutoVerify=$SkipAutoVerify"
    "# Started=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')"
) -Encoding ascii

$cliPath = Join-Path $scriptRoot 'GaYmCLI.exe'
$autoVerifyPath = Join-Path $scriptRoot 'AutoVerify.exe'
if (-not (Test-Path $cliPath)) {
    throw "Missing $cliPath"
}
if (-not (Test-Path $autoVerifyPath)) {
    throw "Missing $autoVerifyPath"
}

Write-Log "LogPath: $LogPath" ([ConsoleColor]::Cyan)
Write-Log "ScriptRoot: $scriptRoot"
Write-Log "Wake regression starting."

$passed = $false
try {
    Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('status') -Label 'Pre-run status' | Out-Null
    Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('wakecheck', "$BaselineSeconds", "$PollIntervalMs", "$DeviceIndex") -Label 'Baseline wakecheck' | Out-Null

    if ($SkipResumePrompt) {
        Write-Log 'Skipping sleep/resume prompt; continuing immediately.' ([ConsoleColor]::Yellow)
    }
    else {
        Write-Log ''
        Write-Log 'Manual step: put the machine to sleep now, wake it, then return here.' ([ConsoleColor]::Cyan)
        $response = Read-Host 'Press Enter after resume to continue, or type Q to abort'
        if ($response.Trim().Equals('q', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Wake regression aborted before post-wake validation.'
        }
    }

    Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('status') -Label 'Post-resume status' | Out-Null
    Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('wakecheck', "$PostWakeSeconds", "$PollIntervalMs", "$DeviceIndex") -Label 'Post-wake wakecheck' | Out-Null

    if ($SkipAutoVerify) {
        Write-Log 'Skipping AutoVerify follow-up.' ([ConsoleColor]::Yellow)
    }
    else {
        Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('on', "$DeviceIndex") -Label 'Enable override' | Out-Null
        try {
            Invoke-LoggedNativeCommand -FilePath $autoVerifyPath -Label 'AutoVerify' | Out-Null
        }
        finally {
            Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('off', "$DeviceIndex") -Label 'Disable override' -AllowFailure | Out-Null
        }
    }

    Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('status') -Label 'Final status' -AllowFailure | Out-Null
    $passed = $true
}
finally {
    Invoke-LoggedNativeCommand -FilePath $cliPath -Arguments @('off', "$DeviceIndex") -Label 'Final override off' -AllowFailure | Out-Null

    if ($passed) {
        Write-Log ''
        Write-Log 'Wake regression PASS.' ([ConsoleColor]::Green)
    }
    else {
        Write-Log ''
        Write-Log 'Wake regression FAILED.' ([ConsoleColor]::Red)
    }
}
