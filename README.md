# GaYmXboxFilter

GaYmXboxFilter is a Windows controller interception project for the Xbox wired HID child path on `VID_045E / PID_02FF`.

The supported MVP architecture is a hybrid stack on the HID child:

- full stack: `GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb`
- upper filter owns authoritative override and output translation
- lower filter owns native-path capture and diagnostics
- primary control channel: `\\.\GaYmXInputFilterCtl`
- lower control channel: `\\.\GaYmFilterCtl` for diagnostics and native-path capture

The repository no longer treats the lower HID-child filter as a dead branch. It is part of the supported runtime architecture because native-path capture and `joy.cpl` / WinMM investigation still depend on it.

This README is the operator and contractor-facing entry point for the supported workflow.

## Supported Scope

- hardware: `HID\VID_045E&PID_02FF&IG_00`
- controller family: wired Xbox One / Series path that enumerates the `02FF` HID child
- build mode: KMDF test/dev workflow
- user-mode contract: shared `GAYM_REPORT` injection model in [ioctl.h](./ioctl.h)

Unsupported for the cleaned MVP:

- parent/composite-only filtering as the supported production path
- USB-sibling lower-filter experiments as the supported production path
- broad multi-device support across DualSense or other experimental stacks
- non-admin mutation of the kernel control surface

## Current Validation Status

- bounded upper-path smoke test: passing
- live supported stack detection: passing
- XInput-facing verifier path: passing
- native `joy.cpl` / WinMM regression path: passing
- hardened control-surface negative tests: passing
- release-check orchestration: passing

The repo is now validated around the supported hybrid stack and the protocol-v1 control ABI.

`release-check.ps1` now exercises install, runtime verification, uninstall, reinstall, and final rollback by default. If Windows requires a reboot after either install pass, the script writes a resume state file, pauses at that boundary, and resumes the pending validation phase automatically on the next run after reboot.

`transition-check.ps1` now supports the same kind of resumable manual flow for unplug/replug and sleep/resume validation. It writes `out\transition-check-state.json`, leaves the override armed across the external transition, and verifies that the driver comes back with override cleared when you rerun the command.

## Repository Layout

- upper driver sources: [driver.c](./driver.c), [device.c](./device.c), [devices.c](./devices.c)
- lower driver sources: [GaYmFilter](./GaYmFilter)
- shared ABI: [ioctl.h](./ioctl.h)
- active tools: [GaYmTestFeeder](./GaYmTestFeeder)
- authoritative scripts: [scripts](./scripts)
- operator docs: [docs](./docs)
- archived experiments and stale workflows: [archive](./archive)

## Authoritative Workflow

Build drivers:

```powershell
.\scripts\build-driver.ps1
```

Build tools:

```powershell
.\scripts\build-tools.ps1
```

Build the release operator bundle:

```powershell
.\scripts\package-bundle.ps1
```

The release bundle keeps the supported verifiers and CLI control path, but omits deep packet sniffers and disables the maintainer-only `jitter` command.

Install the supported hybrid stack:

```powershell
.\scripts\install-driver.ps1
```

If install reports pending configuration or a reboot-required boundary, reboot before running `smoke-test.ps1` or `release-check.ps1` unless the live HID-child stack has already settled to `GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb` and the device no longer reports reboot-required state.

On a fresh Windows test machine using the extracted release bundle, prepare trust and test-mode prerequisites first:

```powershell
.\scripts\prepare-test-machine.ps1
```

If the script reports that testsigning is disabled, enable it and reboot:

```powershell
.\scripts\prepare-test-machine.ps1 -EnableTestSigning
```

Run the smoke test:

```powershell
.\scripts\smoke-test.ps1
```

`install-driver.ps1`, `smoke-test.ps1`, and `release-check.ps1` coordinate through `out\install-driver-state.json` so a reboot-required install on the current boot is reported as an operator checkpoint instead of a misleading runtime failure. If Windows finishes activating the hybrid stack later on the same boot and clears the device reboot-required flag, the stale checkpoint is cleared automatically.

## Key Docs

- architecture: [docs/architecture.md](./docs/architecture.md)
- install: [docs/install.md](./docs/install.md)
- diagnostics: [docs/diagnostics.md](./docs/diagnostics.md)
- rollback: [docs/rollback.md](./docs/rollback.md)

## Notes

- `GAYM_CONTROL_TARGET` remains available as a hidden debug override. It is not part of the supported operator workflow.
- `jitter` is a maintainer-only control intended for debug/full tool builds. It is compiled out of the release bundle CLI.
- Generated binaries, logs, test captures, and packaging output are intentionally excluded from source control. Use [out](./out) or the default build folders as local-only artifacts.
