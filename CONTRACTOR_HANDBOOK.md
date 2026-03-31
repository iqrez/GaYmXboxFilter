# Contractor Handbook

This repository is mid-migration to the
`SPEC-2-GaYm-Controller-IO-Platform-MVP` shape.

## Supported Scope

Build and validate only the supported v1 platform slice:

- Physical adapter: Xbox `02FF` HID child
- Runtime stack: `GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb`
- Producer boundary: `gaym_client`
- Canonical control contract: `GAYM_REPORT`
- Canonical observation contract: `GAYM_OBSERVATION`

Anything outside that matrix is unsupported unless explicitly requested.

The current implementation state is:

- `gaym_client` is the supported producer entry point
- `GAYM_OBSERVATION` is present in the shared ABI
- `out/dev/client/` is the staged client output
- `\\.\GaYmXInputFilterCtl` is the sole control device
- the lower driver has no producer-facing control device

## Source of Truth

- `src/lower/`: lower-filter source and INF
- `src/upper/`: upper-filter source and INF
- `src/shared/`: protocol and semantic ABI
- `src/client/`: producer-facing client library boundary
- `src/tools/GaYmTestFeeder/`: prototype producer and verifier tools
- `scripts/`: supported entry points for build, install, verify, and package
- `archive/`: historical or unsupported material

Do not revive files from `archive/` back into active code without documenting
why.

## Current Safe Workflow

1. Read `PROJECT_STATE.md` before changing code or docs.
2. Use the scripts under `scripts/` for builds and live validation.
3. Treat `gaym_client` as the producer integration boundary.
4. Use the built tools from `out/dev/` or `out/release/` for bounded
   validation.

## Current Architectural Truth

- The upper filter is the only kernel control surface.
- The sole control path is `\\.\GaYmXInputFilterCtl`.
- The lower filter is forwarding plus native observation only.
- Producers must not learn or pack native HID layouts.
- Single-writer ownership is part of the v1 platform contract.
- Semantic observation is a first-class platform contract, not a debug dump.

## Repo Rules

- Preserve the supported hardware boundary.
- Prefer explicit ABI changes with a documented migration path.
- Keep generated files out of source control.
- Do not add new source roots at the repository root.
- Treat `scripts` as the supported operator and maintainer entry point.
