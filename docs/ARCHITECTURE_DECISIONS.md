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

The shared/client split is landed, `gaym_client` is the supported producer
boundary, and the upper driver is the authoritative control and semantic
observation path for the supported Xbox `02FF` stack. The lower path remains
maintainer diagnostics fallback only, and live validation is expected to run
through the scripted workflow under `scripts/`.
