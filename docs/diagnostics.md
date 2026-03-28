# Diagnostics

## Primary Commands

Build the active tools:

```powershell
.\scripts\build-tools.ps1
```

Query live status:

```powershell
.\out\dev\tools\GaYmCLI.exe status
```

The default `status` path is the supported upper control channel.

Run the current hybrid regression:

```powershell
.\out\dev\tools\HybridAutoVerify.exe
```

Native-path regression:

```powershell
.\out\dev\tools\JoyAutoVerify.exe
```

XInput-only regression:

```powershell
.\out\dev\tools\AutoVerify.exe
```

## Maintainer-Only Target Overrides

Upper diagnostics explicitly:

```powershell
$env:GAYM_CONTROL_TARGET = 'upper'
.\out\dev\tools\GaYmCLI.exe status
Remove-Item Env:GAYM_CONTROL_TARGET
```

Lower diagnostics explicitly:

```powershell
$env:GAYM_CONTROL_TARGET = 'lower'
.\out\dev\tools\GaYmCLI.exe status
Remove-Item Env:GAYM_CONTROL_TARGET
```

These overrides are for debugging only. They are not part of the supported operator workflow.

## Supported Diagnostic Tools

Keep:

- `GaYmCLI.exe`
- `GaYmFeeder.exe`
- `AutoVerify.exe`
- `FeederAutoVerify.exe`
- `KeyboardFeederAutoVerify.exe`
- `JoyAutoVerify.exe`
- `HybridAutoVerify.exe`
- `DirectInputAutoVerify.exe`
- `TraceSniffer.exe`
- `SemanticSniffer.exe`
- `JoySniffer.exe`
- `DirectInputSniffer.exe`
- `RawHidSniffer.exe`
- `XInputMonitor.exe`

## What To Collect For A Bug Report

Capture all of the following:

1. `GaYmCLI.exe status`
2. `pnputil` stack output for the `02FF` HID child
3. the exact verifier used and whether it passed or failed
4. whether the controller needed unplug/replug or reboot before the failure

If the issue is native-path only, also include:

- `JoyAutoVerify.exe` output
- `JoySniffer.exe` output if available
- lower-target `GaYmCLI.exe status`

If the issue is XInput-only, include:

- `AutoVerify.exe` output
- `XInputMonitor.exe` observation
- upper-target `GaYmCLI.exe status`

## Runtime Expectations

- upper status should report wrapped XInput-facing activity
- lower status should report semantic/native capture activity
- override should end in `OFF` after bounded tests
- impossible counters or nonsensical query sizes indicate a stale client/server build mismatch

Verifier expectations:

- `AutoVerify.exe` proves the bounded XInput-facing override path still works
- `JoyAutoVerify.exe` is the native `joy.cpl` / WinMM regression check and currently fails on the March 27, 2026 hybrid baseline
- `HybridAutoVerify.exe` is the combined upper/lower regression check and currently fails because the native `joy.cpl` / WinMM leg is still incomplete

## Known Baseline Limitations

As of March 27, 2026:

- `scripts/smoke-test.ps1` passes for the supported live hybrid stack and bounded upper-path injection
- `scripts/smoke-test.ps1 -IncludeRuntimeVerifiers` fails at `JoyAutoVerify.exe`
- a green `status` query does not imply that `joy.cpl` or WinMM parity is already solved

Use the runtime verifiers to track native-path progress. Do not claim full native-path support until `JoyAutoVerify.exe` and `HybridAutoVerify.exe` both pass on the same live stack.
