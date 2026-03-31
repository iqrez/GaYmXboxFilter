# Shared ABI

Active home for protocol and ABI definitions shared across drivers and user-mode
tools.

Current source-of-truth headers:

- `protocol.h`
- `device_ids.h`
- `capability_flags.h`
- `gaym_report.h`
- `gaym_observation.h`
- `ioctl.h`

`ioctl.h` still preserves the prototype lower-driver transport types so the
current runtime can build, but new producer-facing and versioned contract
definitions now live in dedicated headers under this directory.
