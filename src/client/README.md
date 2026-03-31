# Client Library

`src/client/` is the producer-facing boundary for SPEC-2.

Current state:

- `gaym_client.h` exposes a stable user-mode API surface for tools and future
  producers
- the implementation currently preserves the prototype lower-driver runtime
  behavior under the hood
- semantic observation is currently synthesized from the legacy device query
  when the newer kernel observation IOCTL is not available

This keeps the producer/platform split real in source control without claiming
that the upper-driver and single-writer kernel model are already complete.
