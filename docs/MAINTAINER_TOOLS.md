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
- any new lower-driver user-mode control surface

## Control Path

- sole control path: `\\.\GaYmXInputFilterCtl`

The upper path is the authoritative mutation and semantic observation surface
for both operators and maintainers. The lower driver remains in the stack only
for forwarding and native observation capture; it no longer exposes a separate
user-mode control device.

## Repo Rule

Diagnostic binaries and generated logs should stay out of Git. If a maintainer
needs a local capture or sniffer, install it separately on the workstation and
keep it in a local-only location.
