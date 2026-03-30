param(
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $scriptDir = Split-Path -Parent $PSCommandPath
    $candidate = Resolve-Path (Join-Path $scriptDir "..")
    if (Test-Path (Join-Path $candidate ".git")) {
        return $candidate
    }

    return (Get-Location).Path
}

$repoRoot = Get-RepoRoot

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $repoRoot "out\dev\usbxhci-1b1f0-data-model.txt"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$captured = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$content = @"
# USBXHCI 1B1F0 Data Model
Captured: $captured

## Inputs
- out\dev\usbxhci-1b1f0-log-schema.txt
- out\dev\usbxhci-1b1f0-capture-pipeline.txt
- out\dev\usbxhci-1b1f0-parser-rollup-spec.txt

## Hot-Path Event Record
record: UsbXhciProbeEvent
layout_goal:
- fixed-size
- numeric-only hot-path fields
- append-only write pattern

recommended fields:
- Sequence : uint64
- TimestampQpcLike : uint64
- DueTime : int64
- TimerId : uint32
- MatchedArmSequenceHint : uint32
- ContextTag : uint32
- ThreadTag : uint32
- ProbePoint : uint16
- NoteFlags : uint16
- Irql : uint8
- Cpu : uint8
- Reserved0 : uint16
- Period : uint32
- WaitStatus : uint32
- Aux0 : uint64
- Aux1 : uint64

record_size_goal:
- 64 bytes

## Buffer Model
record store:
- fixed-size ring or append buffer of UsbXhciProbeEvent
- monotonic sequence source outside the row payload generation path

buffer session metadata:
- SessionId : uint64
- DroppedRowCount : uint64
- WriteIndex : uint64
- Capacity : uint32
- ActiveFlags : uint32

## ProbePoint Enum
- 1 = ExSetTimer
- 2 = KeWaitForSingleObjectEnter
- 3 = KeWaitForSingleObjectExit
- 4 = KeQueryUnbiasedInterruptTime

## NoteFlags Bits
- 0x0001 = PeriodicTimer
- 0x0002 = OneShotTimer
- 0x0004 = UnmatchedWake
- 0x0008 = CrossCpu
- 0x0010 = TruncatedContext
- 0x0020 = BufferPressureObserved
- 0x0040 = PostWakeSample

## Parser-Side Objects
object: ProbeRow
fields:
- raw event fields
- parsed enum values
- nullable derived timestamps

object: TimerArm
fields:
- sequence
- timer_id
- context_tag
- cpu
- due_time
- period
- arm_timestamp

object: WaitPair
fields:
- enter_row
- exit_row
- wait_status
- enter_to_exit_delta

object: CorrelatedChain
fields:
- confidence
- arm
- optional wait pair
- optional unbiased-time sample
- arm_to_enter_delta
- arm_to_exit_delta
- sample_to_exit_delta
- cadence_window_id

object: RollupSummary
fields:
- total_rows
- parsed_rows
- malformed_rows
- completed_chains
- unmatched_arms
- unmatched_wait_enters
- unmatched_wait_exits
- confidence_counts
- latency_buckets
- clustering_buckets
- cadence_correlation_buckets

## Practical Conclusion
- the hot path should emit one fixed-size UsbXhciProbeEvent record and nothing richer
- all correlated-chain structure belongs on the parser side, not in the probe path
- this model is concrete enough to implement both the kernel buffer and the offline parser without redefining types during coding
"@

Set-Content -Path $OutputPath -Value $content -Encoding ASCII
Write-Host "Wrote $OutputPath"
