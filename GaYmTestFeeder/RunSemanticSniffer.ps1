$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$logPath = Join-Path $scriptDir "semantic_sniffer_$timestamp.log"
$latestPath = Join-Path $scriptDir "latest_semantic_log.txt"

Set-Content -Path $latestPath -Value $logPath

& .\SemanticSniffer.exe --duration-ms 45000 | Tee-Object -FilePath $logPath
