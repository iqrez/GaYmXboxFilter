# Lower Driver

Current home for the authoritative `GaYmFilter` lower-driver prototype.

This tree is intentionally scoped to lower-path diagnostics and native capture.
It remains prototype-grade, and any mutation, override, or producer-facing
control behavior that still lives here is transitional only. That behavior is
expected to move out of the lower driver in a later phase.

Current lower responsibilities:

- native-path observation
- request forwarding and completion handling
- diagnostic capture and trace materialization
- device-family parsing needed to interpret observed native reports

Not lower responsibilities:

- authoritative writer ownership
- producer-facing semantic control
- long-term override state
- public client ABI shaping
