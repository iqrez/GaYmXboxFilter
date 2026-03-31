# Architecture Decisions

## Decision 1: Keep the Hybrid HID-Child Stack

GaYm v1 stays on the real-device interception path:

`GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb`

Reason: it preserves the real controller stack, keeps diagnostics meaningful,
and avoids collapsing the platform into a virtual-device or pure remapper
model.

## Decision 2: Make `gaym_client` The Producer Boundary

Producers do not talk to kernel IOCTLs directly. They talk to
`gaym_client`, which owns ABI negotiation, writer ownership, and semantic
control/observation plumbing.

Reason: the platform needs a stable producer API that survives adapter changes
without exposing native packet layouts.

## Decision 3: Use Single-Writer Control

Only one active writer may own control mutation at a time in v1.

Reason: it prevents merge semantics between manual control, scripts, playback,
and AI loops.

## Decision 4: Keep Raw Diagnostics Maintainer-Only

Native packet capture and adapter-specific forensic helpers stay behind the
maintainer path.

Reason: the public producer contract remains semantic and portable.

## Current State Note

The shared/client split is landed: `gaym_client` exists, `GAYM_OBSERVATION`
is in the shared ABI, and `out/dev/client/` is staged. Control ownership is
still transitional until the upper driver is implemented, so the operator
path remains planned rather than finished.
