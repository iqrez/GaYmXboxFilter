# Rollback And Recovery

## Fast Recovery

Turn override off explicitly:

```powershell
.\out\dev\tools\GaYmCLI.exe off
```

If the feeder is still running, stop it and send a neutral report before exit when possible.

## Uninstall The Hybrid Stack

```powershell
.\scripts\uninstall-driver.ps1
```

This removes both supported GaYm packages:

- `GaYmXboxFilter.inf`
- `GaYmFilter.inf`

If Windows reports pending device configuration, reboot afterward.

Run rollback from an elevated PowerShell session.

## If The HID Child Disappears

Recovery order:

1. unplug the controller
2. wait a few seconds
3. plug it back in
4. if the `02FF` HID child still does not return, reboot once
5. confirm the `02FF` HID child is present again
6. rerun install only after the device has fully re-enumerated

## If Status Returns Impossible Layouts Or Sizes

That usually means the user-mode tool and running driver are on different in-memory builds.

Use this order:

1. uninstall the GaYm packages
2. reboot
3. rebuild drivers and tools
4. reinstall with [scripts/install-driver.ps1](../scripts/install-driver.ps1)
