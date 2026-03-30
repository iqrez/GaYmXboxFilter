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
