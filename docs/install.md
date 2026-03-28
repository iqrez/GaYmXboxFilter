# Build And Install

## Prerequisites

- Visual Studio 2022 Build Tools with WDK integration
- elevated Developer PowerShell
- test-signing environment appropriate for local driver development
- wired Xbox controller that exposes `HID\VID_045E&PID_02FF&IG_00`

If you are validating from the extracted release bundle instead of the full repo, you do not need the build prerequisites. You do still need:

- elevated PowerShell
- the bundled test certificates trusted on the machine
- testsigning enabled if the machine is enforcing normal kernel signing policy

## Preflight

Before install:

1. use an elevated PowerShell session
2. confirm the controller is connected and the `02FF` HID child exists
3. if an older GaYm stack is already installed and behaving oddly, run `.\scripts\uninstall-driver.ps1` first
4. if Windows already reports pending device configuration, reboot before reinstalling

For a fresh test machine using the extracted bundle:

```powershell
.\scripts\prepare-test-machine.ps1
```

If that script reports that testsigning is disabled, enable it and reboot:

```powershell
.\scripts\prepare-test-machine.ps1 -EnableTestSigning
```

If Secure Boot blocks test mode, disable Secure Boot for the test machine or use a production-signed package instead.

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

The release bundle intentionally excludes deep packet sniffers and the maintainer-only `jitter` control path.

## Install The Supported Hybrid Stack

```powershell
.\scripts\install-driver.ps1
```

The same command works from either:

- the full repo root
- the extracted release bundle root

Install order:

1. lower HID-child filter package
2. upper HID-child filter package

The script installs both packages against the supported `02FF` HID child path. If Windows reports pending configuration or reboot requirements, reboot before trusting runtime results.

Repeated installs are supported. If the current pair is already active and up to date, the script reports that no package changes were required instead of treating the run as an error.

## Post-Install Checklist

1. both packages were added successfully
2. the HID child stack shows `GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb`
3. `.\tools\GaYmCLI.exe status` succeeds in the extracted bundle, or `.\out\dev\tools\GaYmCLI.exe status` succeeds in the full repo
4. no pending reboot/configuration warning remains before final runtime verification

## Verify Attachment

```powershell
.\scripts\smoke-test.ps1
```

Full release-style validation, including uninstall/reinstall and final rollback:

```powershell
.\scripts\release-check.ps1
```

If Windows requires a reboot after the reinstall phase, `release-check.ps1` pauses intentionally, writes `out\release-check-state.json`, and exits without treating that reboot boundary as a harness failure. After reboot, rerun the same command and it resumes pass-2 validation automatically.

Manual transition validation uses the same pattern:

```powershell
.\scripts\transition-check.ps1 -InteractiveUnplugReplug
.\scripts\transition-check.ps1 -InteractiveSleepResume
```

Those commands arm the override, write `out\transition-check-state.json`, and tell you to perform the physical transition. After the unplug/replug or sleep/resume is complete, rerun the same command and the script resumes verification instead of starting from scratch.

Manual stack verification:

```powershell
$hidChild = Get-PnpDevice -Class HIDClass | Where-Object { $_.InstanceId -like 'HID\VID_045E&PID_02FF&IG_00*' } | Select-Object -First 1 -ExpandProperty InstanceId
pnputil /enum-devices /instanceid "$hidChild" /stack /drivers
```

Expected stack shape:

```text
GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb
```
