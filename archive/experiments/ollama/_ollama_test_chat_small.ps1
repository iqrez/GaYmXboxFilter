$ProgressPreference = 'SilentlyContinue'

$ErrorActionPreference = 'Stop'

$body = @{
  model = 'gpt-oss-20b:latest'
  messages = @(
    @{ role = 'user'; content = 'Say hello in one sentence.' }
  )
  temperature = 0.2
  max_tokens = 16
  stream = $false
} | ConvertTo-Json -Depth 10

try {
  $res = Invoke-RestMethod -Method Post `
    -Uri 'http://localhost:11434/v1/chat/completions' `
    -ContentType 'application/json' `
    -Body $body `
    -TimeoutSec 120

  if ($null -ne $res.choices -and $res.choices.Count -gt 0) {
    if ($null -ne $res.choices[0].message.content) {
      Write-Output $res.choices[0].message.content
      return
    }
  }

  Write-Output ($res | ConvertTo-Json -Depth 20)
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

