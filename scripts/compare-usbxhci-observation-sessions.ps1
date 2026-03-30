[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BaselineSession,
    [Parameter(Mandatory = $true)]
    [string]$CandidateSession,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Parse-KeyValueLine {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Line
    )

    $separatorIndex = $Line.IndexOf('=')
    if ($separatorIndex -lt 1) {
        return $null
    }

    return [pscustomobject]@{
        Key = $Line.Substring(0, $separatorIndex).Trim()
        Value = $Line.Substring($separatorIndex + 1).Trim()
    }
}

function Parse-LatencyLine {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Line
    )

    if ($Line -notmatch '^(?<name>[^:]+):\s+min=(?<min>-?\d+)\s+p50=(?<p50>-?\d+)\s+p95=(?<p95>-?\d+)\s+max=(?<max>-?\d+)$') {
        return $null
    }

    return [pscustomobject]@{
        Name = $Matches['name'].Trim()
        Min = [int64]$Matches['min']
        P50 = [int64]$Matches['p50']
        P95 = [int64]$Matches['p95']
        Max = [int64]$Matches['max']
    }
}

function Read-ObservationSession {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        throw "Session file not found: $Path"
    }

    $lines = Get-Content -Path $Path
    $header = @{}
    $rollup = @{}
    $latency = @{}
    $section = 'Header'

    foreach ($rawLine in $lines) {
        $line = $rawLine.Trim()
        if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#')) {
            continue
        }

        if ($line -eq '[rollup]') {
            $section = 'Rollup'
            continue
        }

        if ($line -like '[*]') {
            $section = $line.Trim('[', ']')
            continue
        }

        if ($section -eq 'latency' -or ($section -eq 'Rollup' -and $line -like '*: min=*')) {
            $latencyEntry = Parse-LatencyLine -Line $line
            if ($null -ne $latencyEntry) {
                $latency[$latencyEntry.Name] = $latencyEntry
                continue
            }
        }

        $parsedLine = Parse-KeyValueLine -Line $line
        if ($null -eq $parsedLine) {
            continue
        }

        if ($section -eq 'Header') {
            $header[$parsedLine.Key] = $parsedLine.Value
        } else {
            $rollup[$parsedLine.Key] = $parsedLine.Value
        }
    }

    return [pscustomobject]@{
        Path = $Path
        Header = $header
        Rollup = $rollup
        Latency = $latency
    }
}

function Get-IntMetric {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Map,
        [Parameter(Mandatory = $true)]
        [string]$Key
    )

    if (-not $Map.ContainsKey($Key)) {
        return $null
    }

    return [int64]$Map[$Key]
}

function New-DiffLine {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Baseline,
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Candidate
    )

    $delta = $null
    if ($null -ne $Baseline -and $null -ne $Candidate -and
        ($Baseline -is [int] -or $Baseline -is [long]) -and
        ($Candidate -is [int] -or $Candidate -is [long])) {
        $delta = ([int64]$Candidate) - ([int64]$Baseline)
    }

    if ($null -ne $delta) {
        return "{0}: baseline={1} candidate={2} delta={3}" -f $Name, $Baseline, $Candidate, $delta
    }

    return "{0}: baseline={1} candidate={2}" -f $Name, $Baseline, $Candidate
}

function Write-ComparisonReport {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Baseline,
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Candidate,
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $baselineCompleted = Get-IntMetric -Map $Baseline.Rollup -Key 'completed_chains'
    $candidateCompleted = Get-IntMetric -Map $Candidate.Rollup -Key 'completed_chains'
    $baselineMalformed = Get-IntMetric -Map $Baseline.Rollup -Key 'malformed_rows'
    $candidateMalformed = Get-IntMetric -Map $Candidate.Rollup -Key 'malformed_rows'
    $baselineCadenceMatched = Get-IntMetric -Map $Baseline.Rollup -Key 'cadence_matched_chains'
    $candidateCadenceMatched = Get-IntMetric -Map $Candidate.Rollup -Key 'cadence_matched_chains'
    $baselineHigh = Get-IntMetric -Map $Baseline.Rollup -Key 'high'
    $candidateHigh = Get-IntMetric -Map $Candidate.Rollup -Key 'high'

    $baselineArmToExit = $Baseline.Latency['arm_to_exit']
    $candidateArmToExit = $Candidate.Latency['arm_to_exit']
    $baselineEnterToExit = $Baseline.Latency['enter_to_exit']
    $candidateEnterToExit = $Candidate.Latency['enter_to_exit']

    $status = 'Comparable'
    if ($candidateMalformed -gt 0 -or $candidateCompleted -lt $baselineCompleted -or $candidateCadenceMatched -lt $baselineCadenceMatched) {
        $status = 'Regressed'
    }

    $reportLines = @(
        '# USBXHCI Observation Session Comparison'
        ("Captured: {0}" -f (Get-Date).ToString('o'))
        ''
        ('Status={0}' -f $status)
        ('BaselineSession={0}' -f $Baseline.Path)
        ('CandidateSession={0}' -f $Candidate.Path)
        ''
        '[identity]'
        ('BaselineSource={0}' -f $Baseline.Header['Source'])
        ('CandidateSource={0}' -f $Candidate.Header['Source'])
        ('BaselineCaptureTool={0}' -f $Baseline.Header['CaptureTool'])
        ('CandidateCaptureTool={0}' -f $Candidate.Header['CaptureTool'])
        ''
        '[counts]'
        (New-DiffLine -Name 'completed_chains' -Baseline $baselineCompleted -Candidate $candidateCompleted)
        (New-DiffLine -Name 'malformed_rows' -Baseline $baselineMalformed -Candidate $candidateMalformed)
        (New-DiffLine -Name 'cadence_matched_chains' -Baseline $baselineCadenceMatched -Candidate $candidateCadenceMatched)
        (New-DiffLine -Name 'high_confidence' -Baseline $baselineHigh -Candidate $candidateHigh)
        ''
        '[latency]'
        (New-DiffLine -Name 'arm_to_exit.p50' -Baseline $baselineArmToExit.P50 -Candidate $candidateArmToExit.P50)
        (New-DiffLine -Name 'arm_to_exit.p95' -Baseline $baselineArmToExit.P95 -Candidate $candidateArmToExit.P95)
        (New-DiffLine -Name 'enter_to_exit.p50' -Baseline $baselineEnterToExit.P50 -Candidate $candidateEnterToExit.P50)
        (New-DiffLine -Name 'enter_to_exit.p95' -Baseline $baselineEnterToExit.P95 -Candidate $candidateEnterToExit.P95)
        ''
        '[interpretation]'
    )

    if ($status -eq 'Comparable') {
        $reportLines += 'Candidate session preserved chain completion, malformed-row cleanliness, and cadence matching relative to the baseline.'
    } else {
        $reportLines += 'Candidate session widened one or more baseline error bars and should not replace the current producer without explanation.'
    }

    $outputDirectory = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($outputDirectory) -and -not (Test-Path $outputDirectory)) {
        New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    }

    $reportLines -join [Environment]::NewLine | Set-Content -Path $Path -Encoding ASCII
}

$baseline = Read-ObservationSession -Path $BaselineSession
$candidate = Read-ObservationSession -Path $CandidateSession

if (-not $OutputPath) {
    $baselineDirectory = Split-Path -Parent $BaselineSession
    $OutputPath = Join-Path $baselineDirectory 'usbxhci-observation-session-compare.txt'
}

Write-ComparisonReport -Baseline $baseline -Candidate $candidate -Path $OutputPath
Write-Host "Comparison report: $OutputPath"
