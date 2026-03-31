# Upper Driver Status

`src/upper` is now the source-of-truth scaffold for the intended
`GaYmXInputFilter` upper driver.

What is present:

- a dedicated KMDF project file
- a control-device-focused INF package surface
- shared ABI includes for protocol, report, observation, and device IDs
- module placeholders for device core, IOCTL dispatch, read interception, report translation, lifecycle, and trace state

What is not present:

- full runtime injection behavior
- a complete lower-target handoff
- production signing or release hardening

The current implementation is intentionally skeletal but build-oriented, so the
upper driver can become the real control-authority home without reusing the
lower-driver runtime contract as its long-term shape.
