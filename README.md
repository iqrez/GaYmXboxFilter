# GaYm Controller I/O Platform MVP

This repository is being normalized to `SPEC-2-GaYm-Controller-IO-Platform-MVP`.

GaYm is a controller I/O platform, not a one-off Xbox filter. The platform
boundary is semantic:

- producers decide what control state should be sent
- physical device adapters translate that semantic state to and from a real
  controller stack
- `gaym_client` is the supported producer-facing API boundary

The current repo state reflects the finished upper-authority migration:

- `gaym_client` exists under `src/client/`
- `GAYM_OBSERVATION` exists in the shared ABI
- the upper driver is the sole control plane and authoritative semantic
  observation path for the supported Xbox `02FF` stack
- the lower driver is forwarding plus native observation only
- `scripts/` is the supported build, install, verify, and packaging workflow
- live verification relies on `GaYmCLI`, `GaYmFeeder`, `XInputCheck`, and
  `AutoVerify`

The supported MVP target is narrow:

- Physical adapter: Xbox `02FF` HID child
- Runtime stack: `GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb`
- Producer control path: `\\.\GaYmXInputFilterCtl`
- Canonical control contract: `GAYM_REPORT`
- Canonical semantic observation contract: `GAYM_OBSERVATION`

## Current Shape

The repository is mid-migration toward the SPEC-2 layout, with the
shared/client split and upper-driver authority now landed.

- `src/upper/` contains the authoritative upper-driver implementation
- `src/lower/` contains the lower-driver forwarding and native observation path
- `src/shared/` contains the shared ABI, including semantic observation
- `src/tools/` contains the migrated prototype tool sources
- `src/client/` contains `gaym_client`
- `scripts/` contains the supported operator workflow
- `archive/` contains paused or unsupported experiments only

## What Must Be True

- producers speak semantic intent through `gaym_client`
- producers do not pack raw target HID bytes
- only one active writer may own control mutation in v1
- multiple semantic observation readers are allowed
- override stays off by default
- the upper driver is the sole control plane and semantic observation owner
- the lower driver remains observation plus forwarding only
- override is cleared on cleanup, remove, surprise removal, power transition,
  and uninstall
- malformed or ABI-incompatible requests are rejected

## Repo Layout

- `src/` contains active source-of-truth code
- `scripts/` contains the supported build/install/verify workflow
- `docs/` contains architecture and handoff documentation
- `archive/` contains dead or paused experiments only
- `out/` is the staged output root and is ignored in Git

## Verification

Supported verification on the current live stack is:

- bounded control mutation through `GaYmCLI.exe`
- feeder-driven semantic injection through `GaYmFeeder.exe`
- XInput observation through `XInputCheck.exe`
- automated bounded regression through `AutoVerify.exe`

Current tool status:

- `GaYmCLI.exe` is a supported operator and maintainer control tool
- `GaYmFeeder.exe` is a supported producer-side exerciser
- `XInputCheck.exe` is the supported XInput observation monitor
- `AutoVerify.exe` is the supported bounded regression tool and reports result
  modes such as `xinput_direct_pass`, `raw_hid_pass`, and
  `counter_fallback_pass`
- `XInputAutoVerify.exe` is diagnostic-only and is not staged as part of the
  curated dev toolset
- `QuickVerify.exe` is diagnostic-only and not an authoritative pass/fail gate
  on this machine because its in-process XInput polling has produced false
  negatives while `XInputCheck.exe` and the live stack showed valid injected
  XInput state

## Next Steps

1. Expand automated regression around writer conflict and PnP/power edges.
2. Continue splitting large lower-driver and tool modules into stable units.
3. Add more maintainer diagnostics that do not require separate control-plane
   entry points.
