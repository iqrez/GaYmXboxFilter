# Scripts

This directory contains the authoritative PowerShell workflow for build,
install, verify, uninstall, and bundle packaging.

Current state:

- the root batch entrypoints remain curated wrappers around the script-first
  workflow here
- `scripts/` builds from `src/` and stages artifacts under `out/dev/` or
  `out/release/`
- `build-client.ps1` stages the producer-facing client library under the
  selected configuration root
- `build-driver.ps1` stages the paired upper/lower driver packages and curated
  driver outputs under the selected configuration root
- `build-tools.ps1` links against the staged client library and stages the
  curated user-mode toolset
- `install-driver.ps1` and `uninstall-driver.ps1` manage the live supported
  Xbox `02FF` HID-child stack
- `package-bundle.ps1` creates the authoritative curated bundle from the staged
  outputs under `out/dev/` or `out/release/`
- `smoke-test.ps1` runs the staged upper-control CLI/feeder/AutoVerify flow
  from `out/dev/` or `out/release/`

Live install rule:

- if upper or lower driver binaries changed and the live machine is part of
  validation, bump both upper and lower INF `DriverVer` values before running
  `install-driver.ps1`
- signoff requires the active HID instance to bind to the newest staged
  `oem*.inf` pair
- signoff requires `DEVPKEY_Device_IsRebootRequired` to be `FALSE`
