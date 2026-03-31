# Maintainer Tools

Maintainer-only diagnostics are intentionally separated from the producer
surface.

## What Belongs Here

- native-path sniffers
- raw observation helpers
- adapter-specific diagnostics
- trace snapshot tooling
- rollback/debug validation helpers that are not safe for general producers

## What Does Not Belong Here

- producer-facing control APIs
- public semantic mutation helpers
- raw HID packet builders exposed to producers
- release-bundle tools that are meant for normal operator use

## Control Paths

- operator control path: `\\.\GaYmXInputFilterCtl`
- maintainer diagnostic path: `\\.\GaYmFilterCtl`

The operator path is the intended mutation surface for normal use. The
maintainer path exists so diagnostics can remain adapter-specific without
leaking into the producer contract. Control ownership is still transitional
until the upper driver is fully implemented, so do not describe the operator
path as finished product behavior yet.

## Repo Rule

Diagnostic binaries and generated logs should stay out of Git. If a maintainer
needs a local capture or sniffer, install it separately on the workstation and
keep it in a local-only location.
