$ProgressPreference = 'SilentlyContinue'

$body = @{
  model = 'gpt-oss-20b:latest'
  messages = @(
    @{ role = 'user'; content = 'Say hello in one sentence.' }
  )
  temperature = 0.2
  max_tokens = 40
  stream = $false
} | ConvertTo-Json -Depth 10

$res = Invoke-RestMethod -Method Post `
  -Uri 'http://localhost:11434/v1/chat/completions' `
  -ContentType 'application/json' `
  -Body $body

if ($null -ne $res.choices -and $res.choices.Count -gt 0 -and $null -ne $res.choices[0].message.content) {
  $res.choices[0].message.content
} else {
  $res | ConvertTo-Json -Depth 20
}

