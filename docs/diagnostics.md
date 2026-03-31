# Diagnostics

This document defines the supported runtime verification story for the current
SPEC-2 migration state.

## Supported Proof Paths

Use the following tools and interpret them in this order:

1. `GaYmCLI.exe`
2. `GaYmFeeder.exe`
3. `XInputCheck.exe`
4. `AutoVerify.exe`

Current meaning:

- `GaYmCLI.exe status`
  verifies that the supported adapter is present and the upper-owned semantic
  observation surface is reachable
- `GaYmCLI.exe test 0`
  verifies bounded writer acquisition, override, injection, neutral release,
  and override disable
- `GaYmFeeder.exe`
  verifies sustained producer-driven semantic injection through `gaym_client`
- `XInputCheck.exe`
  is the authoritative interactive XInput observation monitor on this machine
- `AutoVerify.exe`
  is the authoritative bounded automated verifier for this repo state and now
  reports explicit modes:
  - `xinput_direct_pass`
  - `raw_hid_pass`
  - `counter_fallback_pass`

## Current Known Limitation

`QuickVerify.exe` is not a supported pass/fail gate on this machine.
`XInputAutoVerify.exe` is also diagnostic-only on this machine and is not part
of the curated staged verifier set.

Why:

- it performs injection and XInput polling in the same process
- in the current environment it has produced false negatives
- those false negatives conflict with live proof from:
  - `GaYmFeeder.exe`
  - `GaYmCLI.exe`
  - `XInputCheck.exe`
  - `AutoVerify.exe`

Treat `QuickVerify.exe` as a diagnostic helper only until it is replaced or
reworked into a multi-process verifier.
Treat `XInputAutoVerify.exe` as a diagnostic helper only until it proves
reliable against the current live stack.

## Recommended Runtime Workflow

1. Run `scripts\smoke-test.ps1`
2. Run `GaYmCLI.exe status`
3. Run `GaYmCLI.exe test 0` or start `GaYmFeeder.exe`
4. Watch `XInputCheck.exe` for packet and state changes
5. Run `AutoVerify.exe`

`smoke-test.ps1` now performs a preflight stale-state recovery check and will
attempt `GaYmCLI.exe off 0` automatically if it detects `Override:ON` before
verification starts.

Expected success signals:

- `GaYmCLI.exe status` shows the supported Xbox `02FF` adapter
- `GaYmCLI.exe test 0` completes without cleanup errors
- `GaYmFeeder.exe` can hold and drive the stack
- `XInputCheck.exe` shows injected stick, trigger, and button changes
- `AutoVerify.exe` returns `PASS`

## Failure Interpretation

Use these distinctions:

- `XInputCheck.exe` shows changes and `AutoVerify.exe` passes:
  treat the stack as XInput-working
- `AutoVerify.exe` reports `counter_fallback_pass`:
  treat bounded mutation as proven, with direct XInput observation still
  environment-sensitive
- `AutoVerify.exe` reports `raw_hid_pass`:
  treat raw HID / native-path observation as the direct proof surface
- `AutoVerify.exe` passes only through raw HID:
  treat raw HID as proven and XInput as environment-sensitive
- `XInputAutoVerify.exe` returns `XINPUT_STATIC` while `XInputCheck.exe` shows changes:
  treat `XInputAutoVerify.exe` as a false negative, not a stack failure
- `QuickVerify.exe` fails while `XInputCheck.exe` shows injected state:
  treat `QuickVerify.exe` as a false negative, not a stack failure
- `GaYmCLI.exe status` shows `Override:ON` after a failed run:
  clear it with `GaYmCLI.exe off 0` and check for a lingering feeder process
