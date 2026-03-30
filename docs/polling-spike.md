# Hardware Polling Spike

## Purpose

This branch exists to answer one narrow question:

Can the wired Xbox `02FF` controller path support real hardware-visible polling control, or is feeder-side injection cadence the only practical timing control on this machine?

This is an experimental research track. It is not the supported production architecture.

The supported product line remains the hybrid HID-child stack:

```text
GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb
```

## What Problem This Spike Is Trying To Solve

The supported hybrid stack now exposes:

- feeder-side injection cadence control
- best-effort HID-class poll-frequency control

The current live stack rejects HID-class poll-frequency control with `ERROR_INVALID_FUNCTION`, which means the supported stack does not currently expose a true hardware polling knob.

That leaves an open technical question:

- Is there a lower USB/composite attachment point where polling can be controlled in a `hidusbf`-style way?

## Non-Goals

This spike is not trying to:

- replace the supported hybrid stack
- rewrite the product architecture
- broaden supported device coverage
- ship a production-ready new filter path in one pass

If the result is “not viable,” that is still a successful spike.

## Hypotheses To Test

1. The controller’s observable polling behavior is owned below the current HID-child stack.
2. Real poll-rate control, if possible, will require a USB/composite-side attachment point rather than the current HID-child pair.
3. HID-class poll-frequency control may exist in theory, but the active `xinputhid` path on this machine does not expose it.

## Success Criteria

Any one of these outcomes is sufficient:

1. Positive proof:
   - a prototype can change hardware-visible polling behavior on the target controller path
   - the effect is observable in captured timing or request cadence

2. Negative proof:
   - the relevant lower path is identified
   - the controller/driver stack refuses or ignores polling control there
   - we can defend the conclusion that feeder cadence is the only practical control on this machine

## Investigation Order

1. Re-capture the live parent/composite and USB/HID-child stack layout for the controller.
2. Identify where `hidusbf`-style logic would actually have to sit to affect endpoint cadence.
3. Separate three possible timing layers:
   - USB endpoint polling interval
   - HID-class poll frequency
   - user-mode/kernel override injection cadence
4. Build a minimal probe for the candidate lower path.
5. Measure whether request cadence or observable input timing changes under that probe.

## Evidence We Already Have

- The supported hybrid HID-child stack works for override, XInput, `joy.cpl`, and transition handling.
- Feeder-side cadence control is real and now operator-visible.
- HID-class poll-frequency control is exposed in tooling, but the active stack rejects it on this machine.
- Earlier `hidusbf` testing changed enumeration/visibility characteristics when attached lower on the parent/composite side.

That last point is why this spike exists at all.

## Expected Artifacts

If the spike is successful, produce:

- one capture note for the live parent/USB/HID stack
- one short design note explaining the actual timing owner on this hardware path
- one minimal prototype or capture probe
- one conclusion:
  - viable next step for real polling control
  - or explicit closure that the supported product should stay with feeder cadence only

Current capture note:

- [polling-capture-note.md](./polling-capture-note.md)

Current measurement tool:

- `CadenceProbe.exe` in debug/full tool builds

Current experimental package paths:

- `scripts\build-usb-child-probe-package.ps1`
- `GaYmFilter\GaYmUsbChildProbe.inf`
- `scripts\install-usb-child-probe.ps1`
- `scripts\uninstall-usb-child-probe.ps1`
- `scripts\build-parent-composite-probe-package.ps1`
- `GaYmFilter\GaYmCompositeProbe.inf`
- `scripts\install-parent-composite-probe.ps1`
- `scripts\uninstall-parent-composite-probe.ps1`

## Current Result

The USB-child probe is now a completed measurement step, and the composite-parent probe is now the first positive cadence-observation result.

Observed with the HID-child lower extension removed:

- HID child:
  - `GaYmXInputFilter -> xinputhid -> HidUsb`
- USB child:
  - `HidUsb -> GaYmFilter -> xboxgip`

Measured result:

- upper path remained steady at roughly `240-242 Hz`
- lower USB-child path exposed a stable semantic packet source
- lower USB-child path did not expose a comparable periodic cadence signal

Interpretation:

- the USB `02FF` child is useful for semantic observation
- it is not sufficient, by itself, to prove or control hardware-visible polling cadence
- the next lower-path candidate is the composite parent `USB\VID_045E&PID_0B12...`

Observed on the isolated composite-parent probe:

- parent stack:
  - `xboxgip -> dc1-controller -> GaYmFilter -> USBPcap -> ACPI -> USBHUB3`
- USB child:
  - `HidUsb -> xboxgip`
- HID child:
  - `GaYmXInputFilter -> xinputhid -> HidUsb`

Measured result:

- upper path still runs at roughly `250 Hz`
- lower parent path now shows a matching live cadence on internal control traffic
- observed lower cadence is approximately `124-127` internal-control requests per `500 ms`
- active semantic source remains:
  - `ioctl=0x00220003`
  - `len=17`

Updated interpretation:

- the composite parent is the first lower path that exposes a live repeating cadence signal comparable to the upper path
- this is the first credible candidate for real hardware-visible polling experimentation on this machine
- the next spike step is no longer discovery of the timing owner
- the next spike step is controlled perturbation of this parent-path cadence to see whether upper-visible polling behavior changes

## Controlled Perturbation Result

That perturbation experiment is now complete.

Method:

- enabled a fixed `5000 us` dev-only jitter on the lower parent probe
- targeted via:
  - `GAYM_CONTROL_TARGET=lower`
  - `GaYmCLI jitter 5000 5000`
- measured before and after with:
  - `CadenceProbe.exe 3 500 lower 0`
  - `CadenceProbe.exe 3 500 upper 0`

Observed baseline:

- lower parent cadence:
  - approximately `124-127` internal-control completions per `500 ms`
- upper cadence:
  - approximately `119-137` `0x8000E00C` device-control completions per `500 ms`

Observed with parent jitter enabled:

- lower parent cadence dropped to roughly `99-104` per `500 ms`
- upper cadence dropped to roughly `48-64` per `500 ms`

Conclusion:

- parent-path completion timing is not just observable
- parent-path completion timing is causal for the upper visible polling behavior on this machine
- this is the first successful below-HID timing intervention in the spike

That does not yet prove direct USB endpoint `bInterval` control.

It does prove that the composite parent path is the first viable place to modulate polling-visible behavior below the supported HID-child stack.

## Parent Jitter Sweep

The next step was to check whether that perturbation scaled smoothly or only appeared at the high end.

Measured with a fixed lower-target jitter sweep:

- `0 us`
- `1000 us`
- `2000 us`
- `3000 us`
- `5000 us`

Method:

- targeted via:
  - `GAYM_CONTROL_TARGET=lower`
  - `GaYmCLI jitter <value> <value>`
- sampled after each change with:
  - `CadenceProbe.exe 3 500 lower 0`
  - `CadenceProbe.exe 3 500 upper 0`

Observed averages per `500 ms` window:

| Lower jitter | Lower parent cadence | Upper cadence |
| --- | ---: | ---: |
| `0 us` | `125.5` | `121.0` |
| `1000 us` | `125.7` | `123.0` |
| `2000 us` | `125.5` | `122.8` |
| `3000 us` | `125.2` | `121.5` |
| `5000 us` | `113.0` | `71.2` |

Interpretation:

- the parent-path timing effect is real
- it is not approximately linear over this range
- `1000-3000 us` produced little or no stable change in upper visible cadence
- `5000 us` still produced a strong drop in both lower parent cadence and upper visible cadence

Current best read:

- the parent path is definitely causal
- the observable transfer function has a threshold or bursty behavior instead of a smooth proportional response
- if real hardware-visible poll control exists here, the next spike should focus on shaping or scheduling the delay more precisely rather than just increasing fixed stall time

## Shaped Scheduler Result

That next scheduling experiment is now complete.

Implementation change:

- replaced the crude per-completion busy stall with a per-device due-time scheduler on the parent probe
- the lower parent path now reserves the next completion slot under a spin lock
- at passive IRQL it sleeps the coarse portion with `KeDelayExecutionThread`
- it only uses `KeStallExecutionProcessor` for the short tail

Measured with the same lower-target sweep:

- `0 us`
- `1000 us`
- `2000 us`
- `3000 us`
- `4000 us`
- `5000 us`

Observed averages per `500 ms` window:

| Lower pacing interval | Lower parent cadence | Upper cadence |
| --- | ---: | ---: |
| `0 us` | `125.3` | `128.7` |
| `1000 us` | `125.5` | `126.7` |
| `2000 us` | `125.8` | `98.2` |
| `3000 us` | `126.5` | `85.7` |
| `4000 us` | `122.8` | `74.8` |
| `5000 us` | `98.3` | `49.5` |

Interpretation:

- the upper-visible effect is now materially smoother than the old fixed-stall experiment
- `2000-5000 us` produces a progressive drop in upper cadence instead of a near-flat response followed by a sudden cliff
- the lower parent cadence still resists change until the higher end of the range, which suggests the parent path is gating or batching requests before the effect fully shows up in its own counters

Current best read:

- shaping the parent-path delay is a better experimental lever than adding a blind fixed stall
- the composite parent is still the only lower path that has shown causal control over upper-visible polling behavior
- this is now strong evidence that any real hardware-polling research should stay on the composite-parent side, not the USB `02FF` child or the supported HID-child product line

## Explicit Target-Rate Experiment

The next step was to move one level above raw microsecond intervals and ask for a desired rate directly.

Implementation:

- kept the kernel surface unchanged
- added a user-mode `GaYmCLI rate ...` experiment on top of the shaped lower pacing knob
- the CLI now:
  - applies a fixed lower interval candidate through `IOCTL_GAYM_SET_JITTER`
  - measures the resulting lower and upper cadence from the live runtime counters
  - searches for the closest match to a requested parent or upper target rate
  - leaves the best interval applied at the end

Supported commands in maintainer builds:

- `GaYmCLI rate parent <hz>`
- `GaYmCLI rate upper <hz>`
- `GaYmCLI rate off`
- `GaYmCLI ratecurve [device_index] [sample_ms] [output_csv]`
- `GaYmCLI ratecurve-stats [runs] [device_index] [sample_ms] [output_csv]`

Observed validation results:

- `GaYmCLI rate parent 245`
  - best interval: `4000 us`
  - measured during sweep: parent `243.3 Hz`, upper `262.0 Hz`
- `GaYmCLI rate upper 150`
  - best interval: `3700 us`
  - measured during sweep: parent `249.3 Hz`, upper `151.3 Hz`

Direct probe after the `upper 150` run:

- lower parent cadence stayed close to baseline:
  - roughly `124-128` internal-control requests per `500 ms`
- upper cadence stayed near the requested target:
  - roughly `70-89` device-control completions per `500 ms`
  - approximately `140-178 Hz`

Interpretation:

- the explicit target-rate layer works better as a user-mode experiment than as another kernel ABI change
- asking for an upper target is now more practical than guessing a raw interval manually
- the lower parent counters still do not fully explain the upper-path response, which reinforces that the parent-side pacing effect propagates upstream in a non-trivial way

For machine-readable captures, `ratecurve` emits:

```text
interval_us,parent_hz,upper_hz
```

and disables the lower pacing experiment again when the sweep finishes.

The next layer now exists too:

- `ratecurve-stats` reruns the same sweep multiple times
- it emits mean/min/max per interval instead of a single noisy sample
- it also restores the lower pacing experiment to `off` when complete

First validation capture:

```text
GaYmCLI ratecurve-stats 2 0 500
```

Observed aggregated output:

| Interval | Parent mean | Parent min-max | Upper mean | Upper min-max |
| --- | ---: | ---: | ---: | ---: |
| `3500 us` | `250.0` | `248.0-252.0` | `224.0` | `168.0-280.0` |
| `4000 us` | `244.0` | `242.0-246.0` | `206.0` | `150.0-262.0` |
| `4500 us` | `217.0` | `212.0-222.0` | `175.0` | `112.0-238.0` |
| `5000 us` | `212.0` | `204.0-220.0` | `178.0` | `132.0-224.0` |

Interpretation:

- the parent-side cadence is comparatively stable once the interval gets large enough to matter
- the upper-visible cadence remains much noisier than the parent counters
- repeated sweeps are therefore necessary before claiming a stable transfer curve at the high end

## Guardrails

- Keep the stable product line untouched.
- Do not merge experimental attach-path changes into the supported branch without separate validation.
- Prefer measurement and proof over large speculative rewrites.
