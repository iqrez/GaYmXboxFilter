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
- native `joy.cpl` / WinMM regression path: still failing and under active investigation

The repo is consolidated around the supported hybrid stack, but native-path parity is not solved yet. Treat the current MVP as a clean engineering baseline, not as a claim that every verifier is green.

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

Install the supported hybrid stack:

```powershell
.\scripts\install-driver.ps1
```

Run the smoke test:

```powershell
.\scripts\smoke-test.ps1
```

## Key Docs

- architecture: [docs/architecture.md](./docs/architecture.md)
- install: [docs/install.md](./docs/install.md)
- diagnostics: [docs/diagnostics.md](./docs/diagnostics.md)
- rollback: [docs/rollback.md](./docs/rollback.md)

## Notes

- `GAYM_CONTROL_TARGET` remains available as a hidden debug override. It is not part of the supported operator workflow.
- Generated binaries, logs, test captures, and packaging output are intentionally excluded from source control. Use [out](./out) or the default build folders as local-only artifacts.
