# GaYm Controller I/O Platform MVP

This repository is being normalized to `SPEC-2-GaYm-Controller-IO-Platform-MVP`.

GaYm is a controller I/O platform, not a one-off Xbox filter. The platform
boundary is semantic:

- producers decide what control state should be sent
- physical device adapters translate that semantic state to and from a real
  controller stack
- `gaym_client` is the supported producer-facing API boundary

The current repo state reflects the shared/client split:

- `gaym_client` exists under `src/client/`
- `GAYM_OBSERVATION` exists in the shared ABI
- `out/dev/client/` is the staged client output
- control ownership is still transitional until the upper driver is fully
  implemented

The supported MVP target is narrow:

- Physical adapter: Xbox `02FF` HID child
- Runtime stack: `GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb`
- Producer control path: `\\.\GaYmXInputFilterCtl`
- Maintainer diagnostic path: `\\.\GaYmFilterCtl`
- Canonical control contract: `GAYM_REPORT`
- Canonical semantic observation contract: `GAYM_OBSERVATION`

## Current Shape

The repository is mid-migration toward the SPEC-2 layout, with the
shared/client split now landed.

- `src/lower/` contains the migrated lower-driver prototype
- `src/shared/` contains the shared ABI, including semantic observation
- `src/tools/GaYmTestFeeder/` contains the migrated prototype tools
- `src/client/` contains `gaym_client`
- `out/dev/client/` is the staged client output
- `src/upper/` is the intended authoritative upper-driver home
- `archive/` contains paused or unsupported experiments only

## What Must Be True

- producers speak semantic intent through `gaym_client`
- producers do not pack raw target HID bytes
- only one active writer may own control mutation in v1
- multiple semantic observation readers are allowed
- override stays off by default
- control ownership remains transitional until the upper driver is fully
  implemented
- override is cleared on remove, surprise removal, power transition, and uninstall
- malformed or ABI-incompatible requests are rejected

## Repo Layout

- `src/` contains active source-of-truth code
- `scripts/` contains the supported build/install/verify workflow
- `docs/` contains architecture and handoff documentation
- `archive/` contains dead or paused experiments only
- `out/` is the staged output root and is ignored in Git

## Next Steps

1. Finish extracting the shared ABI and client boundary.
2. Turn `src/upper/` into the real upper-driver implementation.
3. Keep lower-driver diagnostics adapter-specific and maintainers-only.
4. Replace transitional root scripts with the script-first workflow.
