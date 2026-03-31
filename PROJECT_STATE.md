# Project State

Date: `2026-03-30`

This file records the repository state relative to
`SPEC-2-GaYm-Controller-IO-Platform-MVP`.

## Active Goal

Normalize the prototype into a contractor-executable controller I/O platform
with:

- one supported physical adapter in v1
- one shared semantic control contract
- one minimal semantic observation contract
- one stable producer-facing client boundary
- one clear adapter boundary for future expansion

## Current Reality

- `src/lower/` is the migrated lower-driver prototype tree
- `src/shared/` contains the shared ABI, including `GAYM_OBSERVATION`
- `src/tools/GaYmTestFeeder/` contains the migrated prototype tools and still
  speaks directly to the transitional control path
- `src/client/` contains `gaym_client` as the concrete producer-facing
  library boundary
- `out/dev/client/` is the staged client output
- `src/upper/` does not yet contain the finished upper-driver implementation
- the supported v1 physical adapter is still the Xbox `02FF` HID child
- the runtime stack target remains
  `GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb`

## SPEC-2 Baseline Status

Completed in the current cleanup baseline:

- created the source-of-truth layout under `src/`, `docs/`, `archive/`, and
  `out/`
- archived unrelated experiments and legacy duplicates
- removed tracked build/runtime artifacts from source control
- established transitional scripts that build from `src/` and stage under
  `out/dev/`
- landed the shared/client split and client staging under `out/dev/client/`
- documented the migration boundary in the repo docs

Still open after this baseline:

- finish consolidating the producer-facing API behind `gaym_client`
- keep `GAYM_OBSERVATION` and the rest of the SPEC-2 ABI aligned in shared
  headers
- implement the real upper-driver boundary and writer ownership model
- split the large lower-driver and tool files into stable modules
- replace transitional root scripts with the script-first workflow under
  `scripts/`

## Known Risks

- the lower-driver prototype still owns behavior that should eventually be
  split across `src/lower/`, `src/shared/`, and `src/client/`
- the upper driver is still incomplete, so control ownership remains
  transitional until it is implemented
- root scripts remain transitional and should not be treated as the final
  operator workflow

## Next Recommended Slice

1. add `src/client/` and define the stable producer API
2. finish the `GAYM_REPORT` / `GAYM_OBSERVATION` shared ABI split
3. implement writer-session ownership in the upper driver
4. keep lower diagnostics maintainer-only and adapter-specific
