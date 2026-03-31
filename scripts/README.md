# Scripts

This directory contains the early PowerShell wrappers for the eventual
authoritative build, install, verify, and packaging workflow described in the
MVP spec.

Current state:

- the root batch and PowerShell entrypoints are still usable transitional
  wrappers
- `scripts/` now contains the first script-first equivalents
- both layers should build from `src/` and stage artifacts under `out/dev/`
- `build-client.ps1` stages the producer-facing client library under
  `out/dev/client`
- `build-tools.ps1` now expects and links that staged client library
- `package-bundle.ps1` creates one authoritative curated bundle from the staged
  outputs under `out/dev` or `out/release`
- `smoke-test.ps1` runs the staged client-backed `GaYmCLI.exe status` and
  `GaYmCLI.exe test` flow from `out/dev` or `out/release`
- `uninstall-driver.ps1` removes the staged driver packages when present and
  verifies the supported HID-child stack returns to `HidUsb` without GaYm
  filters where the device is connected
