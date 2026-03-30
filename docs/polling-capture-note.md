# Polling Capture Note

## Snapshot Date

2026-03-29

## Goal

Record the exact active stack layers on the wired Xbox controller path before building any lower-path polling probe.

This note is for the experimental `codex/polling-spike` branch only.

## Live Device Paths

### Parent Composite

Instance ID:

```text
USB\VID_045E&PID_0B12\3032574B30313133323336323435
```

Active stack:

```text
xboxgip
dc1-controller
USBPcap
ACPI
USBHUB3
```

Interpretation:

- This is the composite parent for the controller family.
- If true hardware-visible polling control exists below the HID child, this parent path is one of the main candidates.

### USB Input Child

Instance ID:

```text
USB\VID_045E&PID_02FF&IG_00\00&00&0000CC7A3683ED7E
```

Active stack:

```text
HidUsb
xboxgip
```

Interpretation:

- This is the USB-side game-input child below the HID child.
- This path is a plausible candidate for lower timing observation because it sits below `xinputhid`.
- It is still above the raw hub/host-controller layer, so it may or may not own endpoint timing.

### HID Child

Instance ID:

```text
HID\VID_045E&PID_02FF&IG_00\7&15036beb&d&0000
```

Active stack:

```text
GaYmXInputFilter
xinputhid
GaYmFilter
HidUsb
```

Interpretation:

- This is the supported production stack.
- It is already proven to support:
  - XInput override
  - native-path override
  - feeder cadence control
- It does not currently expose HID-class poll-frequency control on this machine.

## Timing Layers To Separate

The next probe work must distinguish these three layers cleanly.

### 1. USB Endpoint Polling Interval

Questions:

- Is the controller using an interrupt endpoint with a hardware-visible polling interval that can be changed?
- If yes, where in the parent/composite or USB child path is that interval represented or enforced?

Expected ownership:

- likely below the HID child
- possibly parent/composite or lower USB stack

### 2. HID-Class Poll Frequency

Questions:

- Does the active HID stack expose `IOCTL_HID_GET_POLL_FREQUENCY_MSEC` or `IOCTL_HID_SET_POLL_FREQUENCY_MSEC`?

Current result:

- the supported stack rejects HID poll-frequency control on this machine
- so this is not currently the working path for real polling control

### 3. Override Injection Cadence

Questions:

- At what cadence does the feeder inject semantic reports into the supported hybrid stack?

Current result:

- this is already controllable and working through:
  - `GaYmFeeder.exe --poll-rate-hz`
  - `GaYmFeeder.exe --poll-interval-ms`

This is not hardware polling, but it is the currently supported timing knob.

## Working Hypothesis

If real `hidusbf`-style polling control is possible for this controller path, it is more likely to be found on:

1. the parent composite `0B12` path
2. or the USB `02FF` input child

It is unlikely to be owned by the current HID-child hybrid stack, because that stack already rejects the HID poll-frequency ioctl and sits above the more hardware-adjacent timing layers.

## Next Probe Target

The next probe should be minimal and should answer only one question:

- can observed request cadence or completion cadence be changed from the parent/composite or USB-child side?

That probe should prefer measurement over modification first.

Current implementation for that purpose:

```text
CadenceProbe.exe [seconds] [interval_ms] [target] [device_index]
```

Use the lower target first to establish the current HID-child cadence baseline before retargeting any lower attachment experiment.

## First Baseline Measurements

Captured with:

```text
CadenceProbe.exe 3 500 lower 0
CadenceProbe.exe 3 500 upper 0
```

Observed on the current supported stack:

- lower target, idle, no explicit consumer open:
  - essentially flat over 3 seconds
  - no meaningful read/devctl cadence observed
- upper target, idle:
  - consistent `0x8000E00C` device-control cadence
  - approximately `127-142` completions per `500 ms` sample window
  - roughly `250+ Hz` effective upper-path request cadence

Interpretation:

- the upper path is clearly being polled continuously by an active consumer path on this machine
- the current lower HID-child line does not show the same continuous cadence when sampled idle through the lower control plane
- any future USB-child retarget should be compared against this exact upper/lower baseline rather than treated as an isolated reading

## USB-Child Isolation Result

Follow-up topology after removing the HID-child lower extension:

### HID Child

```text
GaYmXInputFilter
xinputhid
HidUsb
```

### USB Input Child

```text
HidUsb
GaYmFilter
xboxgip
```

Measured with:

```text
CadenceProbe.exe 3 500 upper 0
CadenceProbe.exe 3 500 lower 0
```

Observed:

- upper path:
  - still approximately `120-121` completions per `500 ms`
  - roughly `240-242 Hz`
- lower USB-child path:
  - no periodic `read`, `write`, or `devctl` delta during the 3-second sample window
  - only a static semantic source remained:
    - `ioctl=0x00220003`
    - `len=17`

Interpretation:

- the isolated USB-child attachment removes the old control-plane ambiguity cleanly
- even in that clean state, the USB-child path does not expose a live cadence signal comparable to the upper path
- that makes the composite parent the next logical candidate for a true hardware-polling probe

## Composite-Parent Isolation Result

Follow-up topology after removing the USB-child probe and attaching the parent probe:

### Parent Composite

```text
xboxgip
dc1-controller
GaYmFilter
USBPcap
ACPI
USBHUB3
```

### USB Input Child

```text
HidUsb
xboxgip
```

### HID Child

```text
GaYmXInputFilter
xinputhid
HidUsb
```

Measured with:

```text
CadenceProbe.exe 3 500 upper 0
CadenceProbe.exe 3 500 lower 0
```

Observed:

- upper path:
  - approximately `125-139` device-control completions per `500 ms`
  - roughly `250 Hz`
- lower parent path:
  - approximately `124-127` internal-control requests per `500 ms`
  - matching completed request cadence in the same range
  - semantic source remains:
    - `ioctl=0x00220003`
    - `len=17`

Interpretation:

- unlike the isolated USB-child probe, the composite parent exposes a live repeating cadence signal
- that cadence is in the same range as the proven upper-path polling cadence
- this strongly suggests the timing owner for real polling experiments sits on or below the composite-parent `0B12` path
- the next spike step should be a controlled perturbation experiment on the parent-path internal-control flow

## Composite-Parent Perturbation Result

Follow-up measurement after enabling a fixed lower-target jitter on the parent probe:

```text
GAYM_CONTROL_TARGET=lower
GaYmCLI jitter 5000 5000
CadenceProbe.exe 3 500 lower 0
CadenceProbe.exe 3 500 upper 0
```

Observed:

- lower parent cadence dropped from roughly `124-127` to roughly `99-104` per `500 ms`
- upper cadence dropped from roughly `119-137` to roughly `48-64` per `500 ms`

Interpretation:

- this is the first direct causal proof that parent-path timing changes alter the upper visible polling cadence
- the composite parent is therefore the first viable below-HID timing intervention point found in the spike

## Composite-Parent Jitter Sweep

Follow-up sweep with fixed lower-target jitter values:

| Lower jitter | Lower avg / `500 ms` | Upper avg / `500 ms` |
| --- | ---: | ---: |
| `0 us` | `125.5` | `121.0` |
| `1000 us` | `125.7` | `123.0` |
| `2000 us` | `125.5` | `122.8` |
| `3000 us` | `125.2` | `121.5` |
| `5000 us` | `113.0` | `71.2` |

Interpretation:

- `1000-3000 us` does not create a stable observable upper-path change
- `5000 us` still causes a clear drop in both lower and upper cadence
- the parent-path transfer function looks thresholded or bursty, not smoothly proportional

## Composite-Parent Shaped Scheduler Result

Follow-up measurement after replacing the fixed per-completion stall with a due-time scheduler on the parent path.

Measured with:

```text
GAYM_CONTROL_TARGET=lower
GaYmCLI jitter <value> <value>
CadenceProbe.exe 3 500 lower 0
CadenceProbe.exe 3 500 upper 0
```

Observed averages per `500 ms` window:

| Lower pacing interval | Lower avg / `500 ms` | Upper avg / `500 ms` |
| --- | ---: | ---: |
| `0 us` | `125.3` | `128.7` |
| `1000 us` | `125.5` | `126.7` |
| `2000 us` | `125.8` | `98.2` |
| `3000 us` | `126.5` | `85.7` |
| `4000 us` | `122.8` | `74.8` |
| `5000 us` | `98.3` | `49.5` |

Interpretation:

- the shaped scheduler produces a smoother upper-path transfer curve than the old fixed-stall experiment
- `2000-5000 us` now gives a progressive drop in upper cadence instead of a mostly flat response followed by a single cliff at `5000 us`
- the lower parent cadence still bends later than the upper path, which implies the parent-side pacing is visible upstream before it becomes obvious in the parent counters themselves

## Explicit Target-Rate Validation

Follow-up user-mode experiment with the new `GaYmCLI rate ...` command:

```text
GaYmCLI rate parent 245
GaYmCLI rate upper 150
```

Observed during the rate search:

| Requested target | Best lower interval | Measured parent | Measured upper |
| --- | ---: | ---: | ---: |
| `parent 245 Hz` | `4000 us` | `243.3 Hz` | `262.0 Hz` |
| `upper 150 Hz`  | `3700 us` | `249.3 Hz` | `151.3 Hz` |

Direct cadence probe after the `upper 150` run:

- lower parent cadence remained near baseline:
  - roughly `124-128` requests per `500 ms`
- upper cadence remained close to the requested target:
  - roughly `70-89` completions per `500 ms`

Interpretation:

- the new target-rate layer is useful as a user-mode experiment
- upper-target tuning is now materially easier than manual interval guessing
- the gap between nearly flat lower counters and a clearly changed upper cadence confirms that the parent-path pacing effect is real but not fully captured by the lower counter deltas alone

For scripted captures, the current CLI also supports:

```text
GaYmCLI ratecurve [device_index] [sample_ms] [output_csv]
```

which emits CSV rows of:

```text
interval_us,parent_hz,upper_hz
```

and disables the lower pacing experiment again when the sweep finishes.

For repeated captures, it now also supports:

```text
GaYmCLI ratecurve-stats [runs] [device_index] [sample_ms] [output_csv]
```

which emits aggregate CSV rows of:

```text
interval_us,runs,parent_mean_hz,parent_min_hz,parent_max_hz,upper_mean_hz,upper_min_hz,upper_max_hz
```

First validation capture with `ratecurve-stats 2 0 500` showed:

- parent cadence is relatively stable once intervals reach the `4000-5000 us` range
- upper cadence remains visibly noisier over the same range
- repeated captures are therefore more informative than single-pass sweeps when characterizing the upper transfer curve

## Safe-Capped Parent Probe Follow-Up

After one aggressive parent-side stable-rate search produced a watchdog bugcheck, the parent probe was hardened:

- parent pacing cap reduced to `2500 us`
- non-passive completion-path stall capped to `100 us`
- `rate-stable` narrowed to the `0-2500 us` interval set

Guarded validation:

```text
GAYM_CONTROL_TARGET=lower
GaYmCLI rate-stable 100 2 0 500
GaYmCLI jitter off
```

Observed:

- no bugcheck in the guarded run
- best interval selected: `2500 us`
- parent mean: `252.0 Hz`
- upper mean: `257.0 Hz`
- upper range: `256.0-258.0 Hz`

Interpretation:

- the safety cap made the experiment materially safer in this one guarded pass
- it also removed most of the useful control effect
- the earlier large upper-rate bend appears to depend on a level of inline parent-path delay that is not acceptable on the hot path

## Host-Stack Reconnaissance Direction

Broader research into the public `hidusbf` package suggests the meaningful polling change is tied to the host USB stack rather than to post-HID completion delay.

That points the spike toward a new evidence-gathering step:

- capture the active host-stack ancestry for the composite parent
- fingerprint the service image paths and file versions involved
- use that to judge whether host-stack patching or interception is a realistic next experiment on this machine

For that purpose, the repo now includes:

```text
scripts\capture-host-polling-context.ps1
```

First capture on this machine showed:

- composite parent service chain:
  - `dc1-controller -> USBHUB3 -> USBXHCI -> pci -> ACPI`
- active host controller service:
  - `USBXHCI`
- active host controller binary:
  - `C:\Windows\System32\drivers\USBXHCI.SYS`
  - version `10.0.26100.2454`

That matters because it narrows the host-stack research target on this box:

- the next serious `hidusbf`-style research candidate is the `USBXHCI` path
- not `USBPORT.SYS`, which is not the active controller path for this device on this machine

Follow-up `USBXHCI` recon showed:

- there are two present `USBXHCI` controllers on this machine
- the Xbox target is hanging from:
  - `PCI\VEN_8086&DEV_7AE0&SUBSYS_86941043&REV_11\3&11583659&0&A0`
  - `Intel(R) USB 3.20 eXtensible Host Controller - 1.20 (Microsoft)`
- active `USBXHCI.SYS` image:
  - `C:\Windows\System32\drivers\USBXHCI.SYS`
  - version `10.0.26100.2454`
  - SHA256 `B010BFE5944E1C339D0216537137A29F7BE8391B4F0A3729490E44B08D06AF55`

That is the concrete host-controller identity any future patch/intercept experiment would have to target on this box.

## USBXHCI Patchability Assessment

The spike now also includes:

```text
scripts\assess-usbxhci-patchability.ps1
```

That read-only assessment compares the active `USBXHCI.SYS` image on this box against the documented Win11 `hidusbfn` `USBXHCI` range from the local package readme.

Observed:

- target controller:
  - `PCI\VEN_8086&DEV_7AE0&SUBSYS_86941043&REV_11\3&11583659&0&A0`
  - `Intel(R) USB 3.20 eXtensible Host Controller - 1.20 (Microsoft)`
- active image:
  - `C:\Windows\System32\drivers\USBXHCI.SYS`
  - version `10.0.26100.2454`
  - SHA256 `B010BFE5944E1C339D0216537137A29F7BE8391B4F0A3729490E44B08D06AF55`
- documented local `hidusbfn` Win11 range:
  - minimum `10.0.22000.1`
  - maximum `10.0.22621.608`

Interpretation:

- the controller path is clearly hanging from `USBXHCI`, so host-stack work is still the right research layer
- but this exact `USBXHCI.SYS` build is newer than the documented public `hidusbfn` Win11 patch range
- so a future host-stack experiment on this machine would need fresh image-specific recon instead of assuming the published `hidusbf` assumptions transfer directly

## USBXHCI Symbol And Pattern Recon

The spike now also includes:

```text
scripts\capture-usbxhci-symbol-recon.ps1
```

That read-only pass parses the active `USBXHCI.SYS` image directly and records:

- PE header identity
- CodeView debug-directory data
- section hashes for stable anchors
- import tables
- named exports, if any
- ASCII strings matching controller/endpoint/transfer-related keywords

Observed on this machine:

- PE identity:
  - `AMD64`
  - `PE32+`
  - subsystem `Native`
  - entrypoint RVA `0x00055B30`
- CodeView anchor:
  - `RSDS`
  - PDB path `usbxhci.pdb`
- import surface:
  - `HAL.dll`
  - `ntoskrnl.exe`
  - `WDFLDR.SYS`
  - `WppRecorder.sys`
- internal string anchors include:
  - `onecore\drivers\wdm\usb\usb3\usbxhci\sys\endpoint.c`
  - `onecore\drivers\wdm\usb\usb3\usbxhci\sys\controller.c`
  - `onecore\drivers\wdm\usb\usb3\usbxhci\sys\command.c`
  - `onecore\drivers\wdm\usb\usb3\usbxhci\sys\tr.c`
  - `INTERRUPTER_DATA`
  - `ENDPOINT_DATA`
  - `Transfer Ring Tag`

Interpretation:

- the host-layer research target is no longer just "some `USBXHCI` build"
- we now have concrete image anchors for this exact `10.0.26100.2454` binary
- that is enough to support further offline mapping work without touching host-stack behavior yet

## USBXHCI Feature Map Recon

The spike now also includes:

```text
scripts\capture-usbxhci-feature-map.ps1
```

That read-only pass builds a heuristic feature map by:

- parsing the AMD64 runtime-function table from `.pdata`
- collecting category anchors from `.rdata`
- scanning executable sections for RIP-relative references back to those anchors
- clustering likely code regions by feature instead of by raw string presence alone

Observed on this machine:

- runtime function table entries:
  - `1294`
- feature bands:
  - controller/slot/root-hub candidates:
    - `0x00001348 .. 0x00078BE0`
  - endpoint candidates:
    - `0x00007D60 .. 0x00054F74`
  - ring candidates:
    - `0x0000A398 .. 0x0005346C`
  - transfer candidates:
    - `0x00002964 .. 0x00054E60`
  - interrupter candidates:
    - `0x000410B4 .. 0x00081E4D`

Notable function clusters:

- controller/slot/root-hub:
  - `0x0000A640-0x0000A7F4`
  - `0x0000B2A0-0x0000B472`
  - `0x00018AD4-0x00018DCB`
- endpoint:
  - `0x00007D60-0x0000813A`
  - `0x00008250-0x0000844D`
  - `0x000318C4-0x00031987`
- ring/transfer:
  - `0x0000A398-0x0000A62F`
  - `0x00019F40-0x00019FE8`
  - `0x0003D690-0x0003DF6D`
  - `0x00052A74-0x00052C46`
- interrupter:
  - `0x000410B4-0x00041382`
  - `0x0007B5D0-0x0007BAF3`
  - `0x00081980-0x00081E4D`

Interpretation:

- the image-specific target is now narrowed to a finite set of likely controller, endpoint, ring/transfer, and interrupter regions
- the next deeper read-only step can focus on those function bands instead of the whole `USBXHCI.SYS` image

## USBXHCI Cluster Profile Recon

The spike now also includes:

```text
scripts\capture-usbxhci-cluster-profile.ps1
```

That read-only pass takes the current feature-map shortlist and correlates it with:

- parsed import/IAT entries
- direct RIP-relative IAT call/jump sites
- unwind metadata from the AMD64 runtime-function table

Observed on this machine:

- candidate functions profiled:
  - `70`
- parsed IAT entries:
  - `146`
- direct RIP-relative IAT call/jump hits:
  - `1333`

Useful separation signals:

- transfer candidates are dominated by:
  - `KeAcquireSpinLockRaiseToDpc`
  - `KeReleaseSpinLock`
- ring candidates are dominated by:
  - `DbgPrintEx`
  - some `KeStallExecutionProcessor`
- controller/slot/root-hub candidates show more:
  - `KeGetCurrentIrql`
  - `KeQueryUnbiasedInterruptTime`
  - timer APIs
  - `KeDelayExecutionThread`
- interrupter candidates include two pageable `PAGE` functions using:
  - `IoAllocateWorkItem`
  - `ExAllocatePool2`
  - `KeInitializeEvent`
  - `WppRecorder`

Representative candidates:

- controller/control-plane:
  - `0x0001B1F0-0x0001B6D9`
  - `0x0003634C-0x000366BC`
- transfer/hot path:
  - `0x0000320D-0x000038AE`
  - `0x0001144D-0x00011916`
  - `0x000077FC-0x00007B61`
- ring/control transition:
  - `0x00052A74-0x00052C46`
  - `0x000528DC-0x00052A6B`
  - `0x0003D690-0x0003DF6D`
- interrupter:
  - `0x0007B5D0-0x0007BAF3`
  - `0x00081980-0x00081E4D`
  - `0x000410B4-0x00041382`

Interpretation:

- the read-only recon now separates likely hot transfer/ring logic from controller orchestration and pageable interrupter code well enough to guide deeper offline study

## USBXHCI Targeted Transfer And Ring Call Map

The spike now also includes:

```text
scripts\capture-usbxhci-targeted-callmap.ps1
```

That read-only pass narrows the offline map further by:

- taking only the top transfer and ring candidates from the current cluster profile
- scanning those functions for direct `E8` relative internal calls
- scanning those functions for direct RIP-relative `CALL/JMP` IAT sites
- keeping the result function-oriented instead of trying to model the whole image

Observed on this machine:

- selected target functions:
  - `8`
- selected features:
  - `Transfer`
  - `Ring`
- captured call sites:
  - `92`

Top transfer findings:

- `0x0000320D-0x000038AE`
  - strongest direct internal fan-out to:
    - `0x00058B00` (`6` calls)
    - `0x00006BA0` (`3` calls)
  - imported edges dominated by:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
- `0x000077FC-0x00007B61`
  - narrower direct fan-out into nearby helpers:
    - `0x00003FA0`
    - `0x00003C70`
    - `0x00004124`
  - imported edges still dominated by:
    - `KeReleaseSpinLock`
    - `KeAcquireSpinLockRaiseToDpc`
- `0x0001144D-0x00011916`
  - strongest direct internal fan-out to:
    - `0x00010440` (`3` calls)
    - `0x00012400` (`2` calls)
  - imported edges again dominated by:
    - `KeReleaseSpinLock`
    - `KeAcquireSpinLockRaiseToDpc`

Top ring findings:

- `0x000528DC-0x00052A6B`
  - one direct internal edge to:
    - `0x00052254`
  - imports dominated by:
    - `DbgPrintEx`
- `0x00052A74-0x00052C46`
  - one direct internal edge to:
    - `0x00052254`
  - imports dominated by:
    - `DbgPrintEx`
    - one direct `KeStallExecutionProcessor` site
- `0x00052F78-0x00053280`
  - one same-feature edge to:
    - `Ring:0x0005346C`
  - imports dominated by:
    - `DbgPrintEx`

Interpretation:

- the top transfer-event regions look like the better offline study target if the goal is to understand timing-critical host behavior
- they connect into shared `.text` helper code and repeatedly touch the expected spinlock imports
- the narrowed ring/TRB regions look more localized and debug-heavy
- so if there is a future deep offline pass, it should start from the transfer clusters and the helper RVAs they converge on before spending time on the TRB logging bands

## USBXHCI Transfer Helper Convergence Map

The spike now also includes:

```text
scripts\capture-usbxhci-helper-callmap.ps1
```

That read-only pass pivots one layer deeper by:

- taking the transfer-side helper targets discovered by the targeted call map
- ranking them by aggregate inbound calls from transfer-event clusters
- mapping only those helper functions' outgoing direct internal and IAT edges

Observed on this machine:

- transfer-derived helper seeds:
  - `27`
- selected helper functions:
  - `6`
- captured helper call sites:
  - `45`

Top helper seeds:

- `0x00058B00`
  - total inbound transfer calls: `7`
  - unique transfer callers: `2`
- `0x00006BA0`
  - total inbound transfer calls: `3`
- `0x00010440`
  - total inbound transfer calls: `3`
- `0x00004124`
  - total inbound transfer calls: `2`
- `0x000049B4`
  - total inbound transfer calls: `2`
- `0x00058BC0`
  - total inbound transfer calls: `2`

Key helper-to-helper edges:

- `0x00006BA0 -> 0x00058B00` (`3` calls)
- `0x00010440 -> 0x00058B00` (`3` calls)
- `0x00004124 -> 0x00058B00` (`1` call)

Key import signals inside the higher-value helpers:

- `0x00006BA0`
  - repeated:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
  - plus:
    - `IoQueueWorkItem`
    - `KeGetCurrentIrql`
- `0x00010440`
  - repeated:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
    - `KfRaiseIrql`
    - `KeLowerIrql`
  - plus:
    - `IoFreeMdl`

Important structural note:

- `0x00058B00` is only a `6`-byte function in this image
- no direct outgoing internal or IAT calls were captured there
- so it looks more like a tiny shared thunk/dispatch endpoint than the main body of interest

Interpretation:

- the transfer-event clusters do converge into a real helper tier before disappearing into the rest of the image
- that tier then collapses into a tiny shared endpoint at `0x00058B00`
- so the best next offline study targets are the upstream helper bodies:
  - `0x00006BA0`
  - `0x00010440`
  - `0x00004124`
- not the TRB/debug ring bands, and not `0x00058B00` by itself

## USBXHCI Helper Micro Map

The spike now also includes:

```text
scripts\capture-usbxhci-helper-micromap.ps1
```

That read-only pass narrows the scope again by:

- taking only:
  - `0x00006BA0`
  - `0x00010440`
  - `0x00004124`
- decoding their unwind headers
- listing nearby runtime-function neighbors from `.pdata`
- mapping one second-hop layer from their strongest internal callees

Observed on this machine:

- resolved helpers:
  - `3`
- neighbor radius:
  - `2`
- second-hop target budget per helper:
  - `4`

Key local structure:

- `0x00006BA0`
  - neighbors:
    - `0x00006A44`
    - `0x00006E74`
    - `0x00007160`
  - still imports:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
    - `IoQueueWorkItem`
    - `KeGetCurrentIrql`
  - second-hop branches include:
    - `0x00006E74`
    - `0x00007160`
    - `0x00040D38`
- `0x00010440`
  - neighbors:
    - `0x000103CF`
    - `0x00010CD8`
    - `0x00010D60`
  - still imports:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
    - `KfRaiseIrql`
    - `KeLowerIrql`
    - `IoFreeMdl`
  - second-hop branches include:
    - `0x00011240`
    - `0x00022E7C`
    - `0x00058EC0`
- `0x00004124`
  - has one direct internal edge to:
    - `0x00058B00`
  - and one import:
    - `WppAutoLogTrace`
  - so it looks much thinner than the other two helpers

Important second-hop distinctions:

- `0x00006E74`
  - nonpageable
  - no direct IAT edges in this pass
  - pushes further into:
    - `0x00008454`
    - `0x00054F74`
- `0x00040D38`
  - imports:
    - `KeBugCheckEx`
    - `KeGetCurrentProcessorNumberEx`
  - so it reads more like a control/assert branch than a hot transfer body
- `0x00011240`
  - collapses back into:
    - `0x00058B00`
  - plus:
    - `WppAutoLogTrace`
- `0x00022E7C`
  - branches into:
    - `0x0001A7FC`
    - `0x0000DA20`
    - `0x0000DC30`
  - imports:
    - `KeGetCurrentIrql`
    - `VslDeleteSecureSection`

Interpretation:

- the helper tier is now split into two serious inner bodies:
  - `0x00006BA0`
  - `0x00010440`
- `0x00004124` is better treated as a thin wrapper than a main timing candidate
- the best next offline targets are:
  - `0x00006E74`
  - `0x00011240`
  - `0x00022E7C`
- with `0x00058B00` kept only as a shared routing landmark

## USBXHCI Branch Split Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-branch-split.ps1
```

That read-only pass consumes:

- the current helper micro-map
- the current cluster profile

and classifies the three second-hop targets as hot-path continuation, wrapper, or control/assert drift.

Observed on this machine:

- assessed targets:
  - `0x00006E74`
  - `0x00011240`
  - `0x00022E7C`

Result:

- `0x00006E74`
  - classified as:
    - `likely hot-path continuation`
  - because:
    - it stays in nonpageable `.text`
    - it has no direct IAT edges in this pass
    - its internal targets stay in `.text`:
      - `0x00008454`
      - `0x00054F74`
- `0x00011240`
  - classified as:
    - `instrumented wrapper`
  - because:
    - it collapses back into:
      - `0x00058B00`
    - and carries:
      - `WppAutoLogTrace`
- `0x00022E7C`
  - classified as:
    - `control/assert drift`
  - because:
    - it directly reaches controller candidate:
      - `0x0000DC30`
    - and imports:
      - `KeGetCurrentIrql`
      - `VslDeleteSecureSection`

Interpretation:

- the candidate set is now materially reduced
- `0x00006E74` is the only branch that still looks like a credible continuation of a timing-relevant transfer-side path
- `0x00011240` is useful context for tracing/instrumentation behavior, but not the main timing lever
- `0x00022E7C` is now clearly off the main path and into control/security/assert territory

So if the read-only spike continues, the next primary offline target should be:

- `0x00006E74`

## USBXHCI Single-Target Deep Pass

The spike now also includes:

```text
scripts\capture-usbxhci-single-target-deep.ps1
```

That read-only pass centers on the one remaining hot-path candidate:

- `0x00006E74`

and resolves:

- its local runtime-function neighborhood
- its direct internal follow-on targets
- whether those follow-on targets land back in known transfer or endpoint machinery

Observed on this machine:

- primary function:
  - `0x00006E74-0x00007153`
- direct internal targets:
  - `0x00008454`
  - `0x00054F74`
- direct IAT imports:
  - none

Primary neighborhood:

- `0x00006A44-0x00006B99`
- `0x00006BA0-0x00006E6B`
- `0x00006E74-0x00007153`
- `0x00007160-0x000071C8`
- `0x000071C8-0x00007245`

### Follow Target `0x00008454`

- resolved function:
  - `0x00008454-0x000085D3`
- strongest feature-band hit:
  - endpoint
- nearest known candidates:
  - `0x00008250-0x0000844D`
    - `Reset Endpoint`
  - `0x000085E0-0x000087BB`
    - `Stop Endpoint`
- direct internal target:
  - `0x00058B00`
- direct import:
  - `WppAutoLogTrace`

Interpretation:

- `0x00008454` sits directly between two endpoint-tagged candidates
- it still looks like endpoint-adjacent hot machinery, even though the one visible import is tracing
- the internal jump to `0x00058B00` fits the existing helper/thunk routing picture

### Follow Target `0x00054F74`

- resolved function:
  - `0x00054F74-0x000550A4`
- feature-band hits:
  - endpoint
  - interrupter
- nearest known candidate:
  - `0x00054E60-0x00054F1D`
    - transfer
- direct internal targets:
  - `0x00018934`
  - `0x0002C7F8`
- direct imports:
  - `DbgPrint`
  - `KdRefreshDebuggerNotPresent`

Interpretation:

- `0x00054F74` sits on a transfer/endpoint boundary rather than squarely inside the earlier transfer-event cluster set
- its direct imports are debugger-state oriented, not obviously hot transfer-state primitives
- so it looks more like boundary or diagnostic context than the main continuation to prioritize

Overall interpretation:

- the branch out of `0x00006E74` does stay inside endpoint/transfer-adjacent machinery
- but the two exits are not equal:
  - `0x00008454` is the stronger endpoint-side continuation
  - `0x00054F74` is a weaker debug-heavy boundary path
- that narrows the next clean offline target from:
  - `0x00006E74`
- to:
  - `0x00008454`

with `0x00054F74` kept only as secondary seam context.

## USBXHCI Endpoint-Target Deep Pass

The spike now also includes:

```text
scripts\capture-usbxhci-endpoint-target-deep.ps1
```

That read-only pass narrows one layer further by:

- taking the stronger single-target continuation:
  - `0x00008454`
- following only its direct handoff target:
  - `0x00058B00`

Observed on this machine:

- primary function:
  - `0x00008454-0x000085D3`
- direct internal targets:
  - `0x00058B00`
- direct imports:
  - `WppAutoLogTrace`

Primary neighborhood:

- `0x00008180-0x00008244`
- `0x00008250-0x0000844D`
- `0x00008454-0x000085D3`
- `0x000085E0-0x000087BB`
- `0x000087C4-0x00008870`

Interpretation of `0x00008454`:

- it remains centered in an endpoint-heavy local band
- the two nearest named endpoint candidates are still:
  - `Reset Endpoint`
  - `Stop Endpoint`
- so the function still reads like endpoint-adjacent machinery, not like a detached logging island

Follow target `0x00058B00`:

- resolved function:
  - `0x00058B00-0x00058B06`
- section:
  - `.text`
- size:
  - `6`
- direct internal calls:
  - none
- direct IAT calls:
  - none
- nearest known candidates:
  - all far away relative to the endpoint neighborhood around `0x00008454`

Interpretation of `0x00058B00`:

- this still looks like a tiny shared routing thunk
- it does not look like the substantive endpoint or transfer body to prioritize next

Overall interpretation:

- the `0x00008454` branch stays in endpoint-adjacent territory
- the direct handoff into `0x00058B00` does not open a richer hot-path body
- so the next clean offline targets are not:
  - `0x00058B00`
- but instead the neighboring endpoint bodies around `0x00008454`:
  - `0x00008250-0x0000844D`
  - `0x000085E0-0x000087BB`

## USBXHCI Endpoint Neighbor Comparison

The spike now also includes:

```text
scripts\capture-usbxhci-endpoint-neighbor-compare.ps1
```

That read-only pass compares the two neighboring endpoint bodies around `0x00008454`:

- `0x00008250-0x0000844D`
- `0x000085E0-0x000087BB`

Observed on this machine:

- both bodies are similar in size:
  - `509`
  - `475`
- both expose:
  - `8` direct internal targets
  - `0` direct IAT edges

Shared direct internal targets:

- `0x000049B4`
- `0x00005BC0`
- `0x00006A08`
- `0x00006A44`
- `0x0001F9A4`

Unique split:

- `0x00008250` only:
  - `0x0001BF58`
  - `0x000331C8`
- `0x000085E0` only:
  - `0x000087C4`
  - `0x0002BED0`

Interpretation:

- the endpoint neighborhood around `0x00008454` is not a thin wrapper chain
- the two neighboring endpoint bodies are both substantial and structurally similar
- the more important result is that they converge into the same shared helper set
- that shared set is now a better next offline target than studying either endpoint body in isolation

Most useful next targets from this comparison:

- `0x0001F9A4`
  - highest aggregate shared inbound count in this pass
- `0x00006A44`
  - sits immediately adjacent to the earlier hot helper band around `0x00006BA0`
- `0x000049B4`
  - already appeared in the earlier transfer/helper convergence work

## USBXHCI Shared Helper Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-shared-helper-assessment.ps1
```

That read-only pass compares the three shared-helper candidates selected from the endpoint-neighbor comparison:

- `0x0001F9A4`
- `0x00006A44`
- `0x000049B4`

Observed on this machine:

- `0x0001F9A4`
  - function:
    - `0x0001F9A4-0x0001FAF2`
  - assessment:
    - instrumented wrapper
  - direct internal:
    - `0x00058B00`
  - direct import:
    - `WppAutoLogTrace`
- `0x000049B4`
  - function:
    - `0x000049B4-0x00004AB9`
  - assessment:
    - debug-heavy seam
  - direct imports:
    - `DbgPrint`
    - `KdRefreshDebuggerNotPresent`
- `0x00006A44`
  - function:
    - `0x00006A44-0x00006B99`
  - assessment:
    - hot helper continuation
  - direct internal:
    - `0x00006BA0`
    - `0x00007160`
    - `0x00058B00`
  - direct imports:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`

Interpretation:

- `0x0001F9A4` should be demoted to wrapper context
- `0x000049B4` should be demoted to debug-seam context
- `0x00006A44` is the only one of the three that still preserves the hot-path shape from the earlier helper band
- importantly, it reconnects directly into:
  - `0x00006BA0`
  - `0x00007160`

So the next clean offline target is now:

- `0x00006A44`

with immediate follow-on context in:

- `0x00006BA0`
- `0x00007160`

## USBXHCI Hot Helper Band Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-hot-helper-band-assessment.ps1
```

That read-only pass compares the three local members of the surviving hot-helper band:

- `0x00006A44`
- `0x00006BA0`
- `0x00007160`

Observed on this machine:

- `0x00006A44`
  - role:
    - entry helper
  - direct internal:
    - `0x00006BA0`
    - `0x00007160`
    - `0x00058B00`
  - direct imports:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
- `0x00006BA0`
  - role:
    - primary hot body
  - direct internal:
    - `0x00006E74`
    - `0x00007160`
    - `0x0000757C`
    - `0x00040D38`
    - `0x00058B00`
  - direct imports:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
    - `IoQueueWorkItem`
    - `KeGetCurrentIrql`
- `0x00007160`
  - role:
    - thin tail/thunk path
  - direct internal:
    - `0x00058B00`
  - direct imports:
    - none

Interpretation:

- `0x00006A44` should now be treated as the band entry point, not the main body
- `0x00007160` should be treated as a narrow tail into the shared thunk
- `0x00006BA0` is the only member of the band that still carries the full hot-body shape:
  - larger body
  - denser internal fan-out
  - stronger spinlock/work-item import profile

So the next clean offline target is now:

- `0x00006BA0`

with immediate follow-on context in:

- `0x00006E74`
- `0x0000757C`

and `0x00040D38` kept only as likely control/assert context.

## USBXHCI Hot Body Branch Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-hot-body-branch-assessment.ps1
```

That read-only pass compares the three outward branches from the hot body at `0x00006BA0`:

- `0x00006E74`
- `0x0000757C`
- `0x00040D38`

Observed on this machine:

- `0x00006E74`
  - role:
    - endpoint-side hot continuation
  - direct internal:
    - `0x00008454`
    - `0x00054F74`
  - direct imports:
    - none
- `0x0000757C`
  - role:
    - quiet transfer bridge
  - direct internal:
    - none
  - direct imports:
    - none
  - immediate neighbors:
    - `0x000076A0-0x000077F4`
    - `0x000077FC-0x00007B61`
- `0x00040D38`
  - role:
    - control/assert branch
  - direct internal:
    - `0x0001A7FC`
    - `0x00058B00`
  - direct imports:
    - `KeBugCheckEx`
    - `KeGetCurrentProcessorNumberEx`

Interpretation:

- `0x00006E74` remains the only outward branch that still carries explicit hot-path structure
- `0x0000757C` does not itself expose direct fan-out, but it still matters as a compact bridge into the nearby transfer-event neighborhood
- `0x00040D38` stays firmly in the control/assert bucket

So the next clean offline target is now:

- `0x00006E74`

with immediate follow-on context in:

- `0x00008454`
- `0x00054F74`

and `0x0000757C` kept only as a transfer-side bridge marker.

## USBXHCI Endpoint vs Transfer Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-endpoint-vs-transfer-assessment.ps1
```

That read-only pass compares:

- the endpoint-side continuation reached from `0x00006E74`:
  - `0x00008454`
- the transfer-side cluster reached through the bridge neighborhood:
  - `0x000077FC`

Observed on this machine:

- `0x00008454`
  - size:
    - `383`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - role:
    - endpoint-side wrapper/continuation
  - visible direct behavior:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x000077FC`
  - size:
    - `869`
  - direct internal:
    - `10`
  - direct IAT:
    - `5`
  - role:
    - transfer-side hot cluster
  - visible direct behavior includes:
    - `0x00003FA0`
    - `0x00003C70`
    - `0x00004124`
    - `0x000049B4`
    - `0x00005BC0`
    - `0x00007B70`
    - `0x00008878`
    - `KeReleaseSpinLock`
    - `KeAcquireSpinLockRaiseToDpc`

Interpretation:

- the endpoint-side continuation is still real, but it looks structurally thin and instrumented
- the transfer-side cluster is materially richer and closer to the kind of fan-out expected from scheduling/transfer logic
- so from this point in the spike, the transfer side is now the better next reverse-engineering target

So the next clean offline target is now:

- `0x000077FC`

with immediate follow-on context in:

- `0x00007B70`
- `0x00008878`

and `0x00008454` kept only as the thinner endpoint-side continuation.

## USBXHCI Transfer-Cluster Batch

The spike now also includes:

```text
scripts\capture-usbxhci-transfer-cluster-batch.ps1
```

That read-only pass batch-captures and ranks the transfer-side neighborhood around `0x000077FC`:

- `0x000076A0`
- `0x000077FC`
- `0x00007B70`
- `0x00007D60`
- `0x00008878`

Observed on this machine:

- `0x00007D60`
  - size:
    - `986`
  - direct internal:
    - `13`
  - direct IAT:
    - `0`
  - strongest direct reconnects into the shared helper tier:
    - `0x00006A08`
    - `0x00006A44`
    - `0x0001F9A4`
    - `0x0001BF58`
    - `0x000049B4`
    - `0x00005BC0`
  - also advances locally into:
    - `0x00008140`
    - `0x00008180`
- `0x000077FC`
  - remains rich, but ranked second
  - larger fan-out into mixed helper and local transfer targets
- `0x00008878`
  - moderate secondary body
- `0x00007B70`
  - thin WPP/thunk-style edge body
- `0x000076A0`
  - thin WPP/thunk-style edge body

Interpretation:

- the larger batch test changes the target again in a useful way
- `0x000077FC` is not a dead end, but `0x00007D60` is the stronger next body in the same transfer-side neighborhood
- importantly, `0x00007D60` reconnects directly into the helper tier the branch has already mapped
- so this is a stronger place to continue if the goal is to close the loop between transfer-side bodies and the previously identified helper structures

So the next clean offline target is now:

- `0x00007D60`

with immediate follow-on context in:

- `0x00008140`
- `0x00008180`

and `0x000077FC` kept as the upstream transfer-side feeder body.

## USBXHCI Transfer Follow Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-transfer-follow-assessment.ps1
```

That read-only pass compares the two direct local follow-on bodies from `0x00007D60`:

- `0x00008140`
- `0x00008180`

Observed on this machine:

- `0x00008140`
  - size:
    - `55`
  - direct internal:
    - `1`
  - direct IAT:
    - `0`
  - visible direct behavior:
    - `0x00008DA0`
- `0x00008180`
  - size:
    - `196`
  - direct internal:
    - `3`
  - direct IAT:
    - `0`
  - visible direct behavior:
    - `0x00008E74` (`2` calls)
    - `0x00046530`

Interpretation:

- the `0x00007D60` follow-on split is not balanced
- `0x00008140` looks like a small edge helper
- `0x00008180` is the denser and more promising continuation

So the next clean offline target is now:

- `0x00008180`

with immediate follow-on context in:

- `0x00008E74`
- `0x00046530`

and `0x00008140` kept only as the thinner sibling edge.

## USBXHCI Transfer Next-Hop Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-transfer-next-hop-assessment.ps1
```

That read-only pass compares the two direct next-hop bodies from `0x00008180`:

- `0x00008E74`
- `0x00046530`

Observed on this machine:

- `0x00008E74`
  - size:
    - `17`
  - direct internal:
    - `0`
  - direct IAT:
    - `0`
- `0x00046530`
  - size:
    - `333`
  - direct internal:
    - `5`
  - direct IAT:
    - `0`
  - visible direct behavior:
    - `0x0001A7FC`
    - `0x00019AC8`
    - `0x0001AD7C`
    - `0x00058AC0`

Interpretation:

- the `0x00008180` next-hop split is not balanced
- `0x00008E74` is effectively a tiny stub
- `0x00046530` is the only follow-on body with enough structure to justify deeper offline work

So the next clean offline target is now:

- `0x00046530`

with immediate follow-on context in:

- `0x0001A7FC`
- `0x00019AC8`
- `0x0001AD7C`

and `0x00008E74` kept only as the tiny sibling stub.

## USBXHCI Transfer Body Follow Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-transfer-body-follow-assessment.ps1
```

That read-only pass compares the three real next-hop bodies behind `0x00046530`:

- `0x0001A7FC`
- `0x00019AC8`
- `0x0001AD7C`

Observed on this machine:

- `0x0001A7FC`
  - size:
    - `250`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00019AC8`
  - size:
    - `80`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `0x00045A8C`
    - `KdRefreshDebuggerNotPresent`
- `0x0001AD7C`
  - size:
    - `852`
  - direct internal:
    - `10`
  - direct IAT:
    - `5`
  - visible direct behavior includes:
    - `0x0001B0D8`
    - `0x0001B158`
    - `0x00055790`
    - `0x00055864`
    - `0x0000DA20`
    - `0x0000DC30`
    - `KeQueryPerformanceCounter`
    - `EtwActivityIdControl`

Interpretation:

- `0x0001A7FC` is a thin instrumented leg
- `0x00019AC8` is a small debug-leaning helper
- `0x0001AD7C` is the only follow-on body with enough structure and fan-out to justify deeper offline work

So the next clean offline target is now:

- `0x0001AD7C`

with immediate follow-on context in:

- `0x0001B0D8`
- `0x0001B158`
- `0x00055790`
- `0x00055864`

and `0x0000DA20` / `0x0000DC30` kept as likely control-side hooks.

## USBXHCI Transfer Tail-Band Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-transfer-tail-band-assessment.ps1
```

That read-only pass compares the `0x0001AD7C` tail-band split:

- thin tails:
  - `0x0001B0D8`
  - `0x0001B158`
- trace siblings:
  - `0x00055790`
  - `0x00055864`
- shared direct targets:
  - `0x00058AC0`
  - `0x0000C8C0`
- adjacent substantive neighbor:
  - `0x0001B1F0`

Observed on this machine:

- `0x0001B0D8`
  - size:
    - `119`
  - direct internal:
    - `3`
  - visible direct behavior:
    - `0x00058AC0`
    - `0x0000C8C0`
- `0x0001B158`
  - size:
    - `145`
  - direct internal:
    - `2`
  - visible direct behavior:
    - `0x00058AC0`
    - `0x0000C8C0`
- `0x00055790`
  - size:
    - `206`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00055864`
  - size:
    - `202`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00058AC0`
  - size:
    - `30`
  - direct internal:
    - `0`
  - direct IAT:
    - `0`
- `0x0000C8C0`
  - size:
    - `91`
  - direct internal:
    - `0`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `EtwWriteTransfer`
- `0x0001B1F0`
  - size:
    - `1257`
  - direct internal:
    - `24`
  - direct IAT:
    - `11`
  - visible direct behavior includes:
    - `0x0000D210`
    - `0x0001BA28`
    - `0x0001BA64`
    - `0x0001BA8C`
    - `KeQueryUnbiasedInterruptTime`
    - `KeStallExecutionProcessor`
    - `ExAllocateTimer`
    - `ExSetTimer`
    - `ExDeleteTimer`

Interpretation:

- `0x0001B0D8` and `0x0001B158` are only thin mirrored tails
- `0x00055790` and `0x00055864` are trace-heavy side-context
- `0x00058AC0` and `0x0000C8C0` collapse into stub / ETW-side context
- `0x0001B1F0` is the first neighboring body with enough timing and orchestration structure to justify deeper offline work

So the next clean offline target is now:

- `0x0001B1F0`

with immediate follow-on context in:

- `0x0000D210`
- `0x0001BA28`
- `0x0001BA64`
- `0x0001BA8C`

and `0x00058AC0` / `0x0000C8C0` demoted to stub-and-trace side context.

## USBXHCI Timer Body Follow Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-timer-body-follow-assessment.ps1
```

That read-only pass compares the `0x0001B1F0` split:

- direct bridge:
  - `0x0000D210`
- alternate local wrapper ladder:
  - `0x0001BA28`
  - `0x0001BA64`
  - `0x0001BA8C`
- first substantive bodies:
  - `0x0000D258`
  - `0x0001BAC0`

Observed on this machine:

- `0x0000D210`
  - size:
    - `65`
  - direct internal:
    - `1`
  - visible direct behavior:
    - `0x0000D258`
- `0x0001BA28`
  - size:
    - `54`
  - direct internal:
    - `1`
  - visible direct behavior:
    - `0x0001CBB4`
- `0x0001BA64`
  - size:
    - `31`
  - direct internal:
    - `1`
  - visible direct behavior:
    - `0x0001BA8C`
- `0x0001BA8C`
  - size:
    - `34`
  - direct internal:
    - `1`
  - visible direct behavior:
    - self-loop only
- `0x0000D258`
  - size:
    - `664`
  - direct internal:
    - `5`
  - direct IAT:
    - `2`
  - visible direct behavior includes:
    - `0x0001A7FC`
    - `0x0001AD7C`
    - `0x00058AC0`
    - `0x00058BC0`
    - `ExAllocatePool2`
    - `ExFreePoolWithTag`
- `0x0001BAC0`
  - size:
    - `363`
  - direct internal:
    - `6`
  - direct IAT:
    - `0`
  - visible direct behavior includes:
    - `0x0001BF58`
    - `0x0001BC34`
    - `0x0001F9A4`
    - `0x00005BC0`
    - `0x00006A08`

Interpretation:

- the direct `0x0000D210 -> 0x0000D258` side is the primary continuation
- the `0x0001BA28` side is mostly a tiny wrapper ladder before it reaches `0x0001BAC0`
- `0x0001BAC0` is still worth keeping, but as a secondary branch, not the primary next target

So the next clean offline target is now:

- `0x0000D258`

with immediate follow-on context in:

- `0x0001A7FC`
- `0x0001AD7C`
- `0x00058AC0`
- `0x00058BC0`

and `0x0001BAC0` kept as the secondary local branch.

## USBXHCI Next Five Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-next-five-assessment.ps1
```

That read-only pass freezes the current five-target queue:

- primary line:
  - `0x0000D258`
  - `0x0000D59C`
- secondary line:
  - `0x0001BAC0`
  - `0x0001BF58`
- opaque side candidate:
  - `0x00058BC0`

Observed on this machine:

- `0x0000D258`
  - size:
    - `664`
  - direct internal:
    - `5`
  - direct IAT:
    - `2`
  - visible direct behavior includes:
    - `0x0001A7FC`
    - `0x0001AD7C`
    - `0x00058AC0`
    - `0x00058BC0`
    - `ExAllocatePool2`
    - `ExFreePoolWithTag`
- `0x0000D59C`
  - size:
    - `621`
  - direct internal:
    - `7`
  - direct IAT:
    - `1`
  - visible direct behavior includes:
    - `0x0001BF58`
    - `0x0000D210`
    - `0x0001A7FC`
    - `0x00056D8C`
    - `KeStallExecutionProcessor`
- `0x0001BAC0`
  - size:
    - `363`
  - direct internal:
    - `6`
  - direct IAT:
    - `0`
  - visible direct behavior includes:
    - `0x0001BF58`
    - `0x0001BC34`
    - `0x0001F9A4`
    - `0x00005BC0`
    - `0x00006A08`
- `0x0001BF58`
  - size:
    - `294`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00058BC0`
  - size:
    - `682`
  - direct internal:
    - `0`
  - direct IAT:
    - `0`

Interpretation:

- `0x0000D59C` is now the strongest unresolved target in the primary line
- `0x0001BAC0` remains the real secondary branch body
- `0x0001BF58` demotes to trace-heavy follow-on context
- `0x00058BC0` stays opaque and should not outrank the bodies with clearer call structure

So the next clean offline target is now:

- `0x0000D59C`

with immediate follow-on context in:

- `0x0001BF58`
- `0x0000D210`
- `0x0001A7FC`
- `0x00056D8C`

and `0x0001BAC0` kept as the secondary branch body.

## USBXHCI D59C Follow Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-d59c-follow-assessment.ps1
```

That read-only pass resolves the current `0x0000D59C` follow-on set:

- trace-heavy side context:
  - `0x0001BF58`
- bridge:
  - `0x0000D210`
- instrumented side leg:
  - `0x0001A7FC`
- short bridge into larger body:
  - `0x00056D8C`
- resolved body:
  - `0x00056DBC`

Observed on this machine:

- `0x0001BF58`
  - size:
    - `294`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x0000D210`
  - size:
    - `65`
  - direct internal:
    - `1`
  - visible direct behavior:
    - `0x0000D258`
- `0x0001A7FC`
  - size:
    - `250`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00056D8C`
  - size:
    - `42`
  - direct internal:
    - `1`
  - visible direct behavior:
    - `0x00056DBC`
- `0x00056DBC`
  - size:
    - `2125`
  - direct internal:
    - `20`
  - direct IAT:
    - `11`
  - visible direct behavior includes:
    - `0x00001008`
    - `0x0000103C`
    - `0x00001068`
    - `0x00056B50`
    - `0x00058AC0`
    - `KeGetCurrentIrql`
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeQueryTimeIncrement`
    - `KeReleaseSpinLock`
    - `ExAllocatePool2`

Interpretation:

- `0x0001BF58`, `0x0000D210`, and `0x0001A7FC` are all demoted for different reasons:
  - trace-heavy
  - bridge-only
  - instrumented
- `0x00056D8C` is only a short bridge
- `0x00056DBC` is the first substantive unresolved body remaining on this branch

So the next clean offline target is now:

- `0x00056DBC`

with immediate follow-on context in:

- `0x00001008`
- `0x0000103C`
- `0x00001068`
- `0x00056B50`
- `0x00058AC0`

and `0x0001BAC0` kept as the secondary branch body.

## USBXHCI 56DBC Band Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-56dbc-band-assessment.ps1
```

That read-only pass assesses the full `0x00056DBC` neighborhood:

- stub band:
  - `0x00001008`
  - `0x0000103C`
  - `0x00001068`
  - `0x00058AC0`
- helper band:
  - `0x00056B50`
  - `0x00056BA0`
  - `0x00056CA4`
  - `0x00079C58`
- adjacent bodies:
  - `0x00057610`
  - `0x00057748`
  - `0x000585E4`

Observed on this machine:

- `0x00001008`
  - size:
    - `46`
  - direct internal:
    - `0`
  - direct IAT:
    - `0`
- `0x0000103C`
  - size:
    - `37`
  - direct internal:
    - `0`
  - direct IAT:
    - `0`
- `0x00001068`
  - size:
    - `162`
  - direct internal:
    - `0`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `EtwWriteTransfer`
- `0x00058AC0`
  - size:
    - `30`
  - direct internal:
    - `0`
  - direct IAT:
    - `0`
- `0x00056B50`
  - size:
    - `72`
  - direct internal:
    - `0`
  - direct IAT:
    - `0`
- `0x00056BA0`
  - size:
    - `252`
  - direct internal:
    - `1`
  - direct IAT:
    - `6`
  - visible direct behavior includes:
    - `0x00056CA4`
    - `IoQueryFullDriverPath`
    - `RtlInitUnicodeString`
    - `RtlUnicodeStringToAnsiString`
    - `RtlFreeAnsiString`
- `0x00056CA4`
  - size:
    - `174`
  - direct internal:
    - `4`
  - direct IAT:
    - `2`
  - visible direct behavior includes:
    - `0x00079C58`
    - `0x00058BC0`
    - `ExAllocatePool2`
    - `KeInitializeSpinLock`
- `0x00079C58`
  - size:
    - `168`
  - direct internal:
    - `1`
  - direct IAT:
    - `2`
  - visible direct behavior:
    - `EtwRegister`
    - `EtwSetInformation`
- `0x00057610`
  - size:
    - `303`
  - direct internal:
    - `0`
  - direct IAT:
    - `7`
  - visible direct behavior:
    - `EtwUnregister`
    - `ExFreePoolWithTag`
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
- `0x00057748`
  - size:
    - `334`
  - direct internal:
    - `1`
  - direct IAT:
    - `7`
  - visible direct behavior:
    - `DbgPrintEx`
    - `ZwQueryKey`
    - `ExAllocatePoolWithTag`
    - `0x000585E4`
- `0x000585E4`
  - size:
    - `29`
  - direct internal:
    - `0`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `ExFreePoolWithTag`

Interpretation:

- this branch has drifted into:
  - ETW registration
  - ETW unregister
  - path/string handling
  - registry/debug
  - cleanup/free helpers
- it no longer looks like the best place to keep chasing a hot transfer-side continuation

So the next clean offline target is now:

- `0x0001BAC0`

with immediate follow-on context in:

- `0x0001BC34`
- `0x0001BF58`
- `0x0001F9A4`
- `0x00005BC0`
- `0x00006A08`

## USBXHCI Secondary Branch Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-secondary-branch-assessment.ps1
```

That read-only pass resolves the current `0x0001BAC0` branch split:

- substantive body:
  - `0x0001BC34`
- trace-heavy side legs:
  - `0x0001BF58`
  - `0x0001F9A4`
- small reconnecting bridges:
  - `0x00005BC0`
  - `0x00006A08`

Observed on this machine:

- `0x0001BC34`
  - size:
    - `798`
  - direct internal:
    - `7`
  - direct IAT:
    - `2`
  - visible direct behavior includes:
    - `0x00001BE8`
    - `0x00058EC0`
    - `0x00058B00`
    - `IoReuseIrp`
    - `IoSetCompletionRoutineEx`
- `0x0001BF58`
  - size:
    - `294`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x0001F9A4`
  - size:
    - `334`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00005BC0`
  - size:
    - `22`
  - direct internal:
    - `1`
  - visible direct behavior:
    - `0x00006A44`
- `0x00006A08`
  - size:
    - `53`
  - direct internal:
    - `1`
  - visible direct behavior:
    - `0x00041EC0`

Interpretation:

- `0x0001BC34` is the only target in this set that still looks like a substantive IRP/completion body
- `0x0001BF58` and `0x0001F9A4` are both trace-heavy side context
- `0x00005BC0` and `0x00006A08` are both small reconnecting bridges, not primary next targets

So the next clean offline target is now:

- `0x0001BC34`

with immediate follow-on context in:

- `0x00001BE8`
- `0x00058EC0`
- `0x00058B00`

## USBXHCI 1BC34 Follow Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-1bc34-follow-assessment.ps1
```

That read-only pass resolves the direct `0x0001BC34` callee set:

- instrumented leg:
  - `0x00001BE8`
- opaque leg:
  - `0x00058EC0`
- tiny thunk:
  - `0x00058B00`

Observed on this machine:

- `0x00001BE8`
  - size:
    - `341`
  - direct internal:
    - `1`
  - direct IAT:
    - `1`
  - visible direct behavior:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00058EC0`
  - size:
    - `263`
  - direct internal:
    - `0`
  - direct IAT:
    - `0`
- `0x00058B00`
  - size:
    - `6`
  - direct internal:
    - `0`
  - direct IAT:
    - `0`

Interpretation:

- none of the direct `0x0001BC34` callees beats the next same-band body as a deep target
- `0x00001BE8` is instrumented
- `0x00058EC0` is opaque
- `0x00058B00` is a thunk

So the next clean offline target is now:

- `0x0001C090`

## USBXHCI Exhaustive Walk

The spike now also includes:

```text
scripts\capture-usbxhci-exhaustive-walk.ps1
```

That pass turns the current single-target workflow into a bounded exhaustive walk.

Observed on this machine:

- seed set:
  - `0x0001BC34`
- output:
  - `out\dev\usbxhci-exhaustive-walk.txt`
- visited targets:
  - `927`
- class breakdown:
  - `substantive=335`
  - `mixed=294`
  - `trace=141`
  - `bridge=61`
  - `stub=44`
  - `opaque=22`
  - `path-string=16`
  - `thunk=10`
  - `etw=4`
- traversal mode:
  - direct internal targets from substantive and bridge nodes
  - plus substantial same-band neighbors

Coverage check:

- the exhaustive walk includes the earlier hot-path anchors:
  - `0x00006BA0`
  - `0x00006E74`
  - `0x000077FC`
  - `0x00007D60`
  - `0x0001AD7C`
  - `0x0001B1F0`
  - `0x00056DBC`

Interpretation:

- the current read-only traversal frontier is exhausted under the present rules
- this means the branch no longer has a single unresolved "next target" within the reachable subgraph from `0x0001BC34`
- any deeper move now requires either:
  - broader seed selection
  - different traversal rules
  - or a different class of experiment beyond this read-only call-map pass

## USBXHCI Feature Closure

The spike now also includes:

```text
scripts\capture-usbxhci-feature-closure.ps1
```

That pass closes the remaining uncovered feature-map and cluster-profile candidates.

Observed on this machine:

- feature candidates:
  - `70`
- baseline visited:
  - `927`
- missing feature seeds before closure:
  - `15`
- outputs:
  - `out\dev\usbxhci-feature-closure.txt`
  - `out\dev\usbxhci-feature-closure-walk.txt`
- combined visited after closure:
  - `963`
- still missing feature candidates:
  - `0`

Interpretation:

- all current feature candidates are now covered
- after this point, the only remaining uncovered space is general runtime-function space

## USBXHCI Runtime Closure

The spike now also includes:

```text
scripts\capture-usbxhci-runtime-closure.ps1
```

That pass expands the closure from feature candidates to the whole runtime-function table.

Observed on this machine:

- runtime functions:
  - `1294`
- baseline walks:
  - `2`
- baseline visited:
  - `963`
- missing runtime seeds before closure:
  - `331`
- outputs:
  - `out\dev\usbxhci-runtime-closure.txt`
  - `out\dev\usbxhci-runtime-closure-walk.txt`
- combined visited after closure:
  - `1294`
- still missing runtime functions:
  - `0`

Interpretation:

- the current read-only census is now complete for `USBXHCI.SYS` `10.0.26100.2454`
- every runtime function in the image has now been entered at least once under the current deep-pass model
- there is no remaining untested runtime-function target set for this workflow

## USBXHCI Intervention Shortlist

The spike now also includes:

```text
scripts\capture-usbxhci-intervention-shortlist.ps1
```

That pass ranks the best plausible host-side timing intervention points from the full `1294/1294` runtime census.

Observed on this machine:

- parsed targets:
  - `1294`
- output:
  - `out\dev\usbxhci-intervention-shortlist.txt`

Current top candidates:

1. `0x0001B1F0`
   - controller timing/orchestration body
   - imports:
     - `KeQueryUnbiasedInterruptTime`
     - `KeStallExecutionProcessor`
     - `ExAllocateTimer`
     - `ExSetTimer`
     - `ExDeleteTimer`
     - `KeWaitForSingleObject`
2. `0x0001144D`
   - transfer hot-path body
   - imports:
     - `KeAcquireSpinLockRaiseToDpc`
     - `KeReleaseSpinLock`
3. `0x000038CC`
   - transfer hot-path body
   - imports:
     - `KeAcquireSpinLockRaiseToDpc`
     - `KeReleaseSpinLock`
4. `0x00011E20`
   - transfer hot-path body
   - imports:
     - `KeAcquireSpinLockRaiseToDpc`
     - `KeReleaseSpinLock`
5. `0x0003634C`
   - controller timing/orchestration body
   - imports:
     - `KeQueryUnbiasedInterruptTime`
     - `KeDelayExecutionThread`
     - `KeGetCurrentIrql`

Interpretation:

- the branch now has a ranked intervention set, not just complete coverage
- the strongest candidates split cleanly into:
  - controller timing/orchestration bodies
  - transfer hot-path spinlock bodies

## USBXHCI Controller Timing Family Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-controller-timing-family-assessment.ps1
```

That pass compares the two highest-ranked controller timing bodies directly.

Observed on this machine:

- primary bodies:
  - `0x0001B1F0`
  - `0x0003634C`
- output:
  - `out\dev\usbxhci-controller-timing-family-assessment.txt`
- shared direct callees:
  - `0x0000D210`
  - `0x00019AC8`
  - `0x0001A724`
  - `0x0001BA28`
  - `0x0002E390`

Interpretation:

- the two controller timing bodies do share a callee spine
- but the shared descendants mostly demote to:
  - wrappers and bridges
  - trace legs
  - debug-side context
- that means the best controller-family intervention points are still the parent bodies themselves:
  - `0x0001B1F0`
  - `0x0003634C`

## USBXHCI 1B1F0 Exclusive Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-1b1f0-exclusive-assessment.ps1
```

That pass resolves the direct callees unique to the primary controller body.

Observed on this machine:

- reviewed exclusive direct callees:
  - `0x0000BE64`
  - `0x0000BF40`
  - `0x0000C970`
  - `0x0001BA64`
  - `0x0001BA8C`
  - `0x0003FC38`
  - `0x00058B00`
- output:
  - `out\dev\usbxhci-1b1f0-exclusive-assessment.txt`

Interpretation:

- the unique `0x0001B1F0` branch mostly collapses to:
  - wrapper ladders
  - debug/IRQL side legs
  - a thunk
- `0x0003FC38` is the only exclusive descendant that still looks like real timing-side machinery
- the next controller-family target should therefore be:
  - `0x0003FC38`

## USBXHCI 3FC38 Branch Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-3fc38-branch-assessment.ps1
```

That pass resolves the branch under the primary controller-family leaf candidate.

Observed on this machine:

- output:
  - `out\dev\usbxhci-3fc38-branch-assessment.txt`
- reviewed branch nodes:
  - `0x00012BF0`
  - `0x0001BA28`
  - `0x0000BF40`
  - `0x0000C970`
  - `0x0000D210`
  - `0x0003C400`
  - `0x0003FE84`
  - `0x0003FF40`
  - `0x00058B00`

Interpretation:

- `0x0003FC38` remains the leaf timing body in the primary controller family
- the descendants demote into:
  - side bodies
  - flush and completion service legs
  - wrappers
  - debug/IRQL legs
  - a thunk

## USBXHCI 3634C Exclusive Assessment

The spike now also includes:

```text
scripts\capture-usbxhci-3634c-exclusive-assessment.ps1
```

That pass resolves the direct exclusive legs of the secondary controller body.

Observed on this machine:

- output:
  - `out\dev\usbxhci-3634c-exclusive-assessment.txt`
- reviewed exclusive nodes:
  - `0x0001A7FC`
  - `0x0001BA00`
  - `0x0002F834`
  - `0x000303B4`
  - `0x00041388`
  - `0x00056D58`

Interpretation:

- the secondary controller family does not reveal a better deeper timing body either
- `0x0003634C` should be treated as the leaf timing body for that side

Current controller-family leaf set is now:

- primary:
  - `0x0001B1F0`
  - leaf timing descendant `0x0003FC38`
- secondary:
  - `0x0003634C`
