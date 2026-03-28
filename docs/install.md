# Build And Install

## Prerequisites

- Visual Studio 2022 Build Tools with WDK integration
- elevated Developer PowerShell
- test-signing environment appropriate for local driver development
- wired Xbox controller that exposes `HID\VID_045E&PID_02FF&IG_00`

## Preflight

Before install:

1. use an elevated PowerShell session
2. confirm the controller is connected and the `02FF` HID child exists
3. if an older GaYm stack is already installed and behaving oddly, run `.\scripts\uninstall-driver.ps1` first
4. if Windows already reports pending device configuration, reboot before reinstalling

## Build Drivers

```powershell
.\scripts\build-driver.ps1
```

This builds:

- upper package: `GaYmXboxFilter`
- lower package: `GaYmFilter`

Staged outputs are copied to:

- `out/dev/driver-upper`
- `out/dev/driver-lower`

Use `-Configuration Release` to stage into `out/release`.

## Build Tools

```powershell
.\scripts\build-tools.ps1
```

Curated tool output is staged to:

```text
out/dev/tools
```

For an operator-facing release package, build the reduced release bundle:

```powershell
.\scripts\package-bundle.ps1
```

That stages a zip in `out/release` with only the supported driver packages, the curated release tool set, and the operator docs/scripts.

## Install The Supported Hybrid Stack

```powershell
.\scripts\install-driver.ps1
```

Install order:

1. lower HID-child filter package
2. upper HID-child filter package

The script installs both packages against the supported `02FF` HID child path. If Windows reports pending configuration or reboot requirements, reboot before trusting runtime results.

## Post-Install Checklist

1. both packages were added successfully
2. the HID child stack shows `GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb`
3. `.\out\dev\tools\GaYmCLI.exe status` succeeds
4. no pending reboot/configuration warning remains before final runtime verification

## Verify Attachment

```powershell
.\scripts\smoke-test.ps1
```

Manual stack verification:

```powershell
pnputil /enum-devices /connected /instanceid "HID\VID_045E&PID_02FF&IG_00\*" /stack
```

Expected stack shape:

```text
GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb
```
