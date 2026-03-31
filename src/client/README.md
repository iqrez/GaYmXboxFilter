# Client Library

`src/client/` is the producer-facing boundary for SPEC-2.

Current state:

- `gaym_client.h` exposes the stable user-mode API surface for tools and future
  producers
- control, semantic observation, and primary device-info queries resolve
  through the upper control device
- legacy compatibility helpers keep the public API surface stable without
  reintroducing a lower control fallback
- the remaining synthetic observation path is a compatibility fallback used
  only when parsed upper-path observation is not yet available

This keeps the producer/platform split real in source control while preserving
the established public API.
