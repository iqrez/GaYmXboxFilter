# Architecture

## Platform Shape

GaYm is a controller I/O platform with a strict boundary between semantic
producers and physical device adapters.

The supported v1 platform slice is:

- one physical adapter: Xbox `02FF` HID child
- one runtime architecture: `GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb`
- one producer-facing API boundary: `gaym_client`
- one control contract: `GAYM_REPORT`
- one minimal observation contract: `GAYM_OBSERVATION`
- one writer at a time for control mutation

Current implementation state:

- `gaym_client` owns the stable producer-facing API
- the upper driver owns writer sessions, override state, semantic injection,
  primary device info, and semantic observation for the supported target
- semantic observation is materialized from parsed native HID reads on the
  upper path, with no synthetic client fallback
- the lower driver is forwarding plus native observation only
- `scripts/` is the supported Debug/Release build, install, verify, and
  packaging workflow

## Supported Architecture

`Producer App -> gaym_client -> GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb -> Xbox Controller`

Responsibilities:

- Producer apps decide what semantic control state should be sent.
- `gaym_client` owns ABI negotiation, handle selection, writer-session
  handling, and semantic control/observation plumbing.
- The upper driver owns authoritative override state, writer enforcement,
  semantic-to-native injection, XInput read replacement, primary device info,
  and hardware-backed semantic observation on the supported target.
- The lower driver owns native-path forwarding and native observation capture.
- Shared ABI headers own protocol/versioning and the portable semantic shapes.

## Source-of-Truth Layout

```text
/src
  /upper
    driver.c
    device/
      device_core.c
      ioctl_dispatch.c
      read_intercept.c
      report_translate.c
      lifecycle.c
      trace.c
    include/
      driver.h
      upper_device.h
      upper_trace.h
  /lower
    driver.c
    device.c
    devices.c
    include/
      device.h
      devices.h
      driver.h
      logging.h
  /shared
    protocol.h
    ioctl.h
    gaym_report.h
    gaym_observation.h
    device_ids.h
    capability_flags.h
  /client
    gaym_client.h
    gaym_client.c
    gaym_client_session.c
    gaym_client_observation.c
    gaym_client_diag.c
  /tools
    /GaYmTestFeeder
    /cli
    /feeder
    /verify
    /capture
/scripts
/docs
/archive
/out
```

Rules:

- duplicate root-level driver trees are removed after migration
- each driver owns only code unique to its role
- producer-facing types live under `/src/shared` and `/src/client`
- only `/archive` may contain paused or dead experiments
- no tracked binaries, PDBs, objects, catalogs, staged outputs, or IDE machine
  state

## Component Responsibilities

### Producer applications

Own:

- deciding what semantic control state to send
- optional local mapping, scripting, playback, or AI policy logic
- presenting user or programmatic workflows

Do not own:

- driver ABI negotiation
- direct raw IOCTL packing
- target HID packet knowledge
- kernel reconnect and handle-management complexity

### `gaym_client`

Owns:

- stable producer-facing API
- driver ABI negotiation and compatibility checks
- opening and managing control-device handles
- writer-session acquisition and release
- reconnect/retry logic
- bounded send loops and basic rate control
- semantic observation queries
- compatibility handling for legacy API entry points that still resolve through
  the upper control plane

Does not own:

- target-specific kernel packet translation
- native packet parsing that belongs in adapters
- public exposure of raw HID packet layouts

### Upper driver

Owns:

- authoritative override enable/disable state
- authoritative writer-session enforcement for control mutation
- semantic `GAYM_REPORT` to native packet translation
- XInput-facing read replacement / completion logic
- minimal semantic observation materialization from parsed native HID reads
- upper-path safety reset on file cleanup, PnP, and power transitions
- primary status and device-info query surface
- attach-time hardening so the device only reports Xbox `02FF` identity when
  the live target actually matches the supported adapter

Does not own:

- broad unsupported hardware enumeration logic
- producer-specific policy logic
- public raw diagnostic contracts

### Lower driver

Owns:

- native-path request forwarding
- native-path observation capture
- device-family parsing needed to interpret observed native reports
- lower trace and forwarding diagnostics

Does not own:

- any producer-facing control device
- authoritative producer write ownership
- public mutation surface for producers
- public semantic observation authority

### Shared ABI

Owns:

- protocol header
- ABI versioning
- IOCTL numbers
- semantic control structs
- minimal semantic observation structs
- device and capability descriptors
- compile-time layout assertions

## Control Plane

The protocol remains versioned and explicit. The public producer model is
semantic, and the driver transport is internal plumbing.

### Request header

```c
struct GAYM_PROTOCOL_HEADER {
  uint32_t Magic;
  uint16_t AbiMajor;
  uint16_t AbiMinor;
  uint32_t Size;
  uint32_t Flags;
  uint32_t RequestId;
};
```

Rules:

- all variable-size requests start with `GAYM_PROTOCOL_HEADER`
- wrong magic, wrong major ABI, undersized payloads, and unsupported flags are
  rejected
- minor ABI increases may add fields only at the tail
- user-mode client and tools must report ABI mismatch clearly
- public producers do not see or construct target-native packet payloads

### Stable semantic control contract

`GAYM_REPORT` is the canonical producer-to-platform control message.

Rules:

- `GAYM_REPORT` represents desired logical controller state
- `GAYM_REPORT` is a full-state snapshot in v1
- producers send buttons, sticks, and triggers semantically
- producers do not send raw target packet bytes
- target-specific sequencing, report IDs, and native packet rules live in the
  adapter implementation

### Stable semantic observation contract

`GAYM_OBSERVATION` is the minimal producer-readable observation message.

Rules:

- it contains only information that is portable and useful across future
  adapters
- it is not a dump of everything the driver knows
- exotic, transport-specific, or reverse-engineering-specific details stay out
- native/raw snapshots remain separate maintainer concerns, not part of the
  producer control surface

## Writer Ownership

v1 uses strict single-writer ownership on the upper control path.

Rules:

- a producer must acquire a writer session before override enable and
  injection
- inject requests without writer ownership fail
- writer ownership is revoked on file cleanup, device removal, surprise
  removal, D0 exit, or explicit release
- semantic observation and device-info queries may still be read by non-writers
- `\\.\GaYmXInputFilterCtl` is the only control device in the live stack

## Live Validation Rule

When upper or lower driver binaries change and the live machine is part of
validation:

- bump both upper and lower INF `DriverVer` values before staging/installing
- use the scripted install path under `scripts/`
- do not sign off until the active HID instance is bound to the newest staged
  `oem*.inf` pair
- do not sign off while `DEVPKEY_Device_IsRebootRequired` is `TRUE`

## Migration Rule

Treat `src/` as the active source-of-truth layout. New architectural work
should land there, not in archived or duplicate prototype trees.
