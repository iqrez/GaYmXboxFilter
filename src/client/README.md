# Client Library

`src/client/` is the producer-facing boundary for SPEC-2.

Current state:

- `gaym_client.h` exposes a stable user-mode API surface for tools and future
  producers
- the implementation currently preserves the prototype lower-driver runtime
  behavior under the hood
- semantic observation now prefers the upper control path first and only falls
  back to the legacy device query when the kernel observation IOCTL is not
  available
- the remaining synthetic observation path is a compatibility fallback, not
  the primary contract

This keeps the producer/platform split real in source control without claiming
that the upper-driver and single-writer kernel model are already complete.
