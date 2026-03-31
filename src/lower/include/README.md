# Lower Driver Headers

Future home for lower-driver private/public headers such as `lower_device.h`
and `lower_trace.h`.

Keep lower headers limited to native-capture state, diagnostic trace shape, and
device-local helpers. Shared producer ABI, semantic control contracts, and
writer/session declarations should live outside the lower driver.
