# Upper Driver Headers

This directory contains the upper-driver private headers for the
`GaYmXInputFilter` scaffold.

The current split keeps the upper control contract explicit and isolated from
the lower driver:

- `driver.h` declares the driver entry surface
- `upper_device.h` owns the device context and control/observation helpers
- `upper_trace.h` owns trace snapshot types
