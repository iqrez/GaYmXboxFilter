# Project State

Date: `2026-03-31`

This file records the repository state relative to
`SPEC-2-GaYm-Controller-IO-Platform-MVP`.

## Active Goal

Normalize the prototype into a contractor-executable controller I/O platform
with:

- one supported physical adapter in v1
- one shared semantic control contract
- one minimal semantic observation contract
- one stable producer-facing client boundary
- one authoritative upper-driver control and observation path
- one maintainer-only lower diagnostic fallback

## Current Reality

- `src/upper/` is the authoritative upper-driver implementation for the
  supported Xbox `02FF` HID child
- `src/lower/` remains the native observation and maintainer-diagnostic
  fallback layer
- `src/shared/` contains the shared ABI, including `GAYM_OBSERVATION`
- `src/client/` contains `gaym_client` as the stable producer-facing boundary
- producer-facing tools now prefer the upper control device first
- `scripts/` is the supported build, install, uninstall, verify, and packaging
  workflow
- `out/dev/` and `out/release/` are the staged output roots
- the supported v1 physical adapter remains the Xbox `02FF` HID child
- the runtime stack target remains
  `GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb`

## SPEC-2 Baseline Status

Completed in the current baseline:

- created the source-of-truth layout under `src/`, `docs/`, `archive/`, and
  `out/`
- archived unrelated experiments and legacy duplicates
- removed tracked build/runtime artifacts from source control
- landed the shared/client split and staged client outputs under the configured
  build roots
- established the script-first workflow under `scripts/` for Debug and Release
  builds, live install/uninstall, smoke verification, and bundle packaging
- made the upper driver the authoritative owner of writer sessions, override
  state, semantic report injection, primary device info, and semantic
  observation for the supported Xbox `02FF` target
- narrowed the lower driver to native observation and maintainer diagnostics,
  with lower-only fallback retained for unsupported diagnostic IOCTLs
- aligned the curated Release bundle and live verification flow with the
  staged `out/release` path

Still open after this baseline:

- split the large lower-driver and tool sources into more stable modules
- extend automated regression coverage around writer conflicts and PnP/power
  transitions
- retire the remaining lower-control compatibility paths once diagnostics are
  explicitly isolated from producer-facing flows

## Known Risks

- the supported v1 hardware target is still intentionally narrow: one Xbox
  `02FF` HID child
- the lower diagnostic path still exists and must not regain producer-facing
  responsibility
- live driver validation requires paired upper/lower INF `DriverVer` bumps, or
  Windows may keep older active packages bound to the device stack

## Next Recommended Slice

1. Expand automated regression coverage for writer conflict, D0 transitions,
   and surprise removal.
2. Continue module extraction in `src/lower/` and the migrated tool sources.
3. Remove the remaining lower-control compatibility surface after diagnostics
   no longer depend on it.
