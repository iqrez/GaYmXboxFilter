# Upper Device Modules

This directory now holds the minimal buildable upper-driver module split for
`GaYmXInputFilter`.

The files here are intentionally narrow:

- `device_core.c` sets up the device shell and queue
- `ioctl_dispatch.c` owns the upper control-device contract surface
- `read_intercept.c` is the placeholder for upper-path read replacement
- `report_translate.c` is the placeholder for semantic-to-native shaping
- `lifecycle.c` holds D0 and removal transitions
- `trace.c` materializes trace and observation snapshots
