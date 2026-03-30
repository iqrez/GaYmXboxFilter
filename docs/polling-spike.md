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

## Guardrails

- Keep the stable product line untouched.
- Do not merge experimental attach-path changes into the supported branch without separate validation.
- Prefer measurement and proof over large speculative rewrites.
