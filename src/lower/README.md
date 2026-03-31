# Lower Driver

`src/lower/` is the source-of-truth home for the `GaYmFilter` lower filter.

This tree is intentionally scoped to lower-path forwarding and native
observation capture only.

Current lower responsibilities:

- request forwarding to the real HID target
- completion handling for native read traffic
- native observation capture and parsing
- device-family parsing needed to interpret observed native reports

Not lower responsibilities:

- any producer-facing control device
- authoritative writer ownership
- producer-facing semantic control
- override state management
- public client ABI shaping
