# Client Library

`src/client/` is the producer-facing boundary for SPEC-2.

Current state:

- `gaym_client.h` exposes the stable user-mode API surface for tools and future
  producers
- control, semantic observation, and primary device-info queries now prefer
  the upper control device first
- the lower path remains available only as a diagnostic or compatibility
  fallback for unsupported IOCTLs
- the remaining synthetic observation path is a compatibility fallback used
  only when parsed upper-path observation is not yet available

This keeps the producer/platform split real in source control while preserving
the lower diagnostic fallback without making it the primary contract.
