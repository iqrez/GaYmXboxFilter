$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Stop'

$body = @{
  model = 'gpt-oss-20b:latest'
  messages = @(
    @{ role = 'user'; content = 'Say hello in one sentence.' }
  )
  temperature = 0.2
  max_tokens = 8
  stream = $false
} | ConvertTo-Json -Depth 10

try {
  $res = Invoke-RestMethod -Method Post `
    -Uri 'http://localhost:11434/v1/chat/completions' `
    -ContentType 'application/json' `
    -Body $body `
    -TimeoutSec 180

  $res | ConvertTo-Json -Depth 12
}
catch {
  Write-Output "ERROR: $($_.Exception.Message)"
  if ($_.Exception.Response -ne $null) {
    try {
      $stream = $_.Exception.Response.GetResponseStream()
      if ($stream -ne $null) {
        $reader = New-Object System.IO.StreamReader($stream)
        Write-Output $reader.ReadToEnd()
      }
    } catch {}
  }
}

