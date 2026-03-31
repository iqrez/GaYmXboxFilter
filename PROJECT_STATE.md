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
- one forwarding-and-observation lower filter with no producer control surface

## Current Reality

- `src/upper/` is the authoritative upper-driver implementation for the
  supported Xbox `02FF` HID child
- `src/lower/` is the lower forwarding and native-observation layer
- `src/shared/` contains the shared ABI, including `GAYM_OBSERVATION`
- `src/client/` contains `gaym_client` as the stable producer-facing boundary
- producer-facing tools resolve control, semantic observation, and primary
  device-info queries through the upper control device only
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
- completed native upper-path semantic observation so `GAYM_OBSERVATION`
  always resolves from parsed native HID traffic rather than a synthetic
  client fallback
- removed the lower control device and all transitional lower control logic
- narrowed the lower driver to native observation and request forwarding only
- aligned the curated Release bundle and live verification flow with the staged
  `out/release` path

Still open after this baseline:

- split the large lower-driver and tool sources into more stable modules
- extend automated regression coverage around writer conflicts and PnP/power
  transitions
- decide whether currently unsupported diagnostic IOCTLs should be implemented
  on the upper path or explicitly retired from tooling

## Known Risks

- the supported v1 hardware target is still intentionally narrow: one Xbox
  `02FF` HID child
- unsupported diagnostic IOCTLs must not quietly regress into new producer
  entry points on the lower stack
- live driver validation requires paired upper/lower INF `DriverVer` bumps, or
  Windows may keep older active packages bound to the device stack
- upper semantic observation now depends on obtaining at least one real native
  report from the live stack and should fail rather than synthesize state

## Signing & WHQL readiness - test cert in place

- added a repo-local self-signed test code-signing certificate under
  `scripts/` for repeatable package signing
- added `scripts/sign-driver.ps1` as the authoritative upper/lower package
  signing step for `out/dev/` and `out/release/`
- kept the signed package layout mirrored so install and uninstall workflows
  can consume either the top-level staged INF files or the `package/`
  subdirectory copies
- updated the release build flow so the staged packages are signed before the
  bundle refresh and install verification steps
- the repo is now structurally ready for WHQL submission prep, pending the
  actual Microsoft submission artifacts and attestation/signing process

## Next Recommended Slice

1. Expand automated regression coverage for writer conflict, D0 transitions,
   and surprise removal.
2. Continue module extraction in `src/lower/` and the migrated tool sources.
3. Decide the long-term fate of snapshot/capture compatibility APIs that now
   resolve only through the upper control plane.
