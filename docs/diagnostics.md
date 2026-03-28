# Diagnostics

## Primary Commands

Build the active tools:

```powershell
.\scripts\build-tools.ps1
```

Maintainer builds keep the full diagnostic surface, including packet sniffers and the `GaYmCLI.exe jitter` command.

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

Full release-style orchestration:

```powershell
.\scripts\release-check.ps1
```

If the reinstall phase reaches a reboot boundary, rerun the same command after reboot. The script auto-detects `out\release-check-state.json` and resumes the pending post-reboot validation phase instead of starting over.

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

## Transition Harness

Default transition coverage:

```powershell
.\scripts\transition-check.ps1
```

Optional manual physical-transition coverage:

```powershell
.\scripts\transition-check.ps1 -InteractiveUnplugReplug
.\scripts\transition-check.ps1 -InteractiveSleepResume
```

Those switches intentionally require operator participation. They validate that override state clears cleanly across unplug/replug and sleep/resume without baking risky power-control behavior into the script itself.

Manual transition flow:

1. run the chosen command once to arm the override and write `out\transition-check-state.json`
2. perform the physical transition
3. rerun the same command
4. the script auto-resumes, verifies override cleared, and removes the state file on success

## Supported Diagnostic Tools

Full maintainer tool set:

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

Release bundle tool set:

- `GaYmCLI.exe`
- `GaYmFeeder.exe`
- `AutoVerify.exe`
- `FeederAutoVerify.exe`
- `KeyboardFeederAutoVerify.exe`
- `JoyAutoVerify.exe`
- `HybridAutoVerify.exe`
- `SecurityAutoVerify.exe`
- `DirectInputAutoVerify.exe`
- `XInputMonitor.exe`

Release bundle restrictions:

- `GaYmCLI.exe jitter` is not available
- deep sniffers and packet probes are excluded from the bundle
- lower-target overrides remain undocumented maintainer-only diagnostics, not operator workflow

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
- `JoyAutoVerify.exe` is the native `joy.cpl` / WinMM regression check and currently passes on the supported hybrid stack
- `HybridAutoVerify.exe` is the combined upper/lower regression check and currently passes on the supported hybrid stack
- `SecurityAutoVerify.exe` proves the hardened control surface rejects restricted or malformed access as expected

## Known Baseline Limitations

As of March 28, 2026:

- `scripts/release-check.ps1` passes on the supported live hybrid stack
- `scripts/release-check.ps1` can pause at the reboot boundary during reinstall, then resume pass-2 validation automatically on the next run after reboot
- `scripts/transition-check.ps1` can still report `SKIPPED` when Windows blocks in-session `pnputil /restart-device` for this HID child
- `scripts/transition-check.ps1` now supports resumable manual unplug/replug and sleep/resume checks through `out\transition-check-state.json`
- a green `status` query still does not replace the runtime verifiers; use them to validate end-to-end behavior after stack changes
