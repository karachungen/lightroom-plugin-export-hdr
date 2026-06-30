# Releasing Ultra HDR Export

Public releases are tagged `vX.Y.Z` and published as GitHub Releases with platform-specific zip archives.

## Version source of truth

[`ExportHDR.lrplugin/Info.lua`](ExportHDR.lrplugin/Info.lua):

```lua
VERSION = { major = 2, minor = 0, revision = 0, build = 0 }
```

| Field | Meaning |
|-------|---------|
| `major` / `minor` / `revision` | Semver shown in release titles, Git tags, and Lightroom Plug-in Manager |
| `build` | Keep at `0` (Lightroom SDK 4-field `VERSION` table; not used for tagging) |

Git tags use **pure semver**: `v` + `major.minor.revision` (e.g. `v2.0.0`).

## Changelog

Maintain [`CHANGELOG.md`](CHANGELOG.md) using [Keep a Changelog](https://keepachangelog.com/) sections.

Before each release:

1. Move notes from `## Unreleased` into a new exact section header:

   ```markdown
   ## v2.0.1
   ```

2. Use the same tag string the workflow will publish (`v` + semver from `Info.lua`).

3. Leave `## Unreleased` in place for the next cycle (it can be empty).

Older changelog sections may use the legacy `v1.0.0-rN` format; keep them as historical archive.

CI fails if the matching changelog section is missing.

## Release checklist

1. Land user-facing changes on `master` / `main`.
2. Bump `VERSION` in `Info.lua`:
   - Increment `revision` for patch releases.
   - Bump `minor` or `major` when behavior warrants it.
   - Keep `build = 0`.
3. Add the matching `## vX.Y.Z` section to `CHANGELOG.md`.
4. Push. When relevant paths change, [`.github/workflows/release-plugin.yml`](.github/workflows/release-plugin.yml) will:
   - ensure the tag does not already exist
   - extract the changelog section
   - build the plugin zip
   - publish the GitHub Release

To cut a release without other code changes, bump `Info.lua` and update `CHANGELOG.md`, then push.

## Local validation

```bash
chmod +x ./scripts/build_release_notes.sh

# Validate parsing + changelog section for the version in Info.lua
./scripts/build_release_notes.sh --check-only

# Dry-run release notes for an existing historical tag
./scripts/build_release_notes.sh --check-only --tag v2.0.0

# Dry-run release notes for an existing historical tag
./scripts/build_release_notes.sh --dry-run --tag v2.0.0 --commit "$(git rev-parse HEAD)" --run-id local
cat RELEASE_NOTES.md
```

## Install instructions for users

Published releases include platform, Lightroom requirement, install steps, commit link, workflow run link, and a link to the full changelog.
