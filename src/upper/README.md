# Upper Driver Status

`src/upper` is the source-of-truth home for the authoritative
`GaYmXInputFilter` upper driver.

What is present:

- a dedicated KMDF project file and control-device package surface
- shared ABI integration for protocol, report, observation, and device IDs
- single-writer enforcement on `\\.\GaYmXInputFilterCtl`
- override enable/disable and semantic `GAYM_REPORT` injection
- XInput read interception and replacement for the supported target
- primary device-info and semantic observation queries backed by upper-driver
  state
- semantic observation materialized from parsed completed reads instead of
  mirrored directly from injection input
- lifecycle cleanup that clears writer ownership and override state on file
  cleanup, D0 exit, and surprise removal
- attach-time hardening so the device only reports Xbox `02FF` identity when
  the live attachment matches the supported target

What is not present:

- broad unsupported hardware support
- maintainer-facing raw diagnostic APIs exposed as producer contracts
- full retirement of the lower diagnostic fallback

The current implementation is the authoritative producer control and semantic
observation path for the supported Xbox `02FF` stack. The lower control device
remains available only as a maintainer-diagnostic fallback for unsupported
IOCTLs and compatibility paths.
