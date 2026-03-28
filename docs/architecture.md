# Supported Architecture

## Product Decision

The supported product is a hybrid HID-child filter stack for the Xbox `02FF` path, not an upper-only or lower-only product.

Supported runtime stack:

```text
GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb
```

Supported hardware target:

```text
HID\VID_045E&PID_02FF&IG_00
```

## Why This Is The Supported Shape

The repository contains several historical attachment experiments:

- parent/composite upper-filter work
- USB-sibling lower-filter work
- HID-child upper-filter work
- HID-child lower-filter work

Only the HID-child hybrid stack matches the current evidence:

- upper filter is the authoritative output/override path for wrapped XInput-facing packets
- lower filter remains necessary for native-path capture and `joy.cpl` / WinMM investigation

The lower HID-child line is not archived as dead code. It is part of the supported runtime architecture.

Operationally, this architecture won because:

- upper-only work proved sufficient for XInput-facing override
- upper-only work did not reliably carry the native `joy.cpl` / WinMM path on this machine
- lower HID-child capture remained the only credible native-path observation point without abandoning the working upper override path

Current implementation status:

- the hybrid stack is the supported architecture and build target
- the upper XInput-facing override path is working
- the lower native-path capture path is live
- the native `joy.cpl` / WinMM-side output path is still incomplete

## Source Of Truth

Upper driver:

- [driver.c](../driver.c)
- [device.c](../device.c)
- [devices.c](../devices.c)
- [GaYmXboxFilter.inf](../GaYmXboxFilter.inf)
- [GaYmXboxFilter.vcxproj](../GaYmXboxFilter.vcxproj)

Lower driver:

- [GaYmFilter/driver.c](../GaYmFilter/driver.c)
- [GaYmFilter/device.c](../GaYmFilter/device.c)
- [GaYmFilter/devices.c](../GaYmFilter/devices.c)
- [GaYmFilter/GaYmFilter.inf](../GaYmFilter/GaYmFilter.inf)
- [GaYmFilter.vcxproj](../GaYmFilter.vcxproj)

Shared ABI:

- [ioctl.h](../ioctl.h)

## Ownership Matrix

Upper filter owns:

- authoritative override state
- semantic-to-native output translation
- wrapped XInput-facing packet rewriting
- primary operator control path through `\\.\GaYmXInputFilterCtl`

Lower filter owns:

- native-path observation and semantic capture
- `joy.cpl` / WinMM-side investigation support
- lower/native telemetry snapshots
- secondary diagnostic control path through `\\.\GaYmFilterCtl`

## Control Plane

Primary supported control path:

```text
\\.\GaYmXInputFilterCtl
```

Secondary diagnostic path:

```text
\\.\GaYmFilterCtl
```

Operator behavior:

- normal control and injection goes to the upper control device
- lower control is used for diagnostics, semantic capture, and native-path debugging
- `GAYM_CONTROL_TARGET` is a hidden debugging override, not a supported operator input

## Data Model

User-mode injects exactly one semantic report shape:

- [GAYM_REPORT](../ioctl.h)

Translation rules:

- upper driver translates `GAYM_REPORT` to the wrapped/native output representations it owns
- lower driver captures and parses native traffic into semantic snapshots for diagnostics and future reconciliation

## Non-Production / Archived Paths

The following are explicitly not part of the supported runtime architecture:

- composite-parent upper-filter experiments
- USB-sibling lower-filter experiments
- legacy one-off helper scripts that deploy only the lower filter
- ad hoc probing tools that are not part of the curated build path

Those artifacts are preserved under [archive](../archive) for reference instead of staying mixed into the main workflow.
