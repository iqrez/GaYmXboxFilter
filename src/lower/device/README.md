# Lower Device Modules

Future home for the lower-driver modules that stay on the native/diagnostic
side of the boundary:

- `device_core.c`
- `ioctl_dispatch.c`
- `native_capture.c`
- `report_parse.c`
- `lifecycle.c`
- `trace.c`

These modules should remain focused on observing, parsing, and exporting the
native path. Writer/session ownership, override orchestration, and producer-
facing semantic control do not belong in the lower driver and should move out
later.
