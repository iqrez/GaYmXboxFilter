# User-Mode Tools

Active home for the migrated user-mode prototype sources.

Current state:

- `GaYmTestFeeder/` contains the pre-split prototype tool sources.
- `cli/`, `feeder/`, `verify/`, and `capture/` are placeholders for the final
  spec-shaped split.
- Supported verification on this machine is `GaYmCLI.exe`, `GaYmFeeder.exe`,
  `XInputCheck.exe`, and `AutoVerify.exe`.
- `XInputCheck.exe` runs continuously by default until any key is pressed and
  accepts `--duration <seconds>` plus `--port <0-3>` when a bounded run is
  more convenient.
- `QuickVerify.exe` and `XInputAutoVerify.exe` are diagnostic-only.
- Diagnostic-only verifiers are not staged by the curated build scripts.
