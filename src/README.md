# Source Layout Target

This directory is the active source-of-truth layout for the MVP migration.

Current population:

- `lower/` contains the migrated lower-driver prototype.
- `shared/` contains the first shared ABI header.
- `tools/` contains the migrated user-mode prototype sources.
- `upper/` is intentionally a placeholder until a real upper-driver source tree
  exists.

New structural work should extend `src/`, not recreate legacy root-level trees.
