# Releasing Ultra HDR Export

Public releases are tagged `vX.Y.Z` and published as GitHub Releases with platform-specific zip archives. Each release also has a short **vibe-coding** codename (GitHub title `{codename} · vX.Y.Z`). Emojis in the name are encouraged.

Merging to `master` / `main` does **not** build or publish. Cut a release from **Actions → Release Lightroom plugin (macOS + Windows) → Run workflow**.

## Version source of truth

[`ExportHDR.lrplugin/Info.lua`](ExportHDR.lrplugin/Info.lua):

```lua
VERSION = { major = 3, minor = 0, revision = 0, build = 0 }
```

| Field | Meaning |
|-------|---------|
| `major` / `minor` / `revision` | Semver shown in Git tags, Lightroom Plug-in Manager, and the GitHub title suffix |
| `build` | Keep at `0` (Lightroom SDK 4-field `VERSION` table; not used for tagging) |

Git tags use **pure semver**: `v` + `major.minor.revision` (e.g. `v3.0.0`). The joke name is not stored in `Info.lua`.

## Changelog

Maintain [`CHANGELOG.md`](CHANGELOG.md) using [Keep a Changelog](https://keepachangelog.com/) sections.

Before each release:

1. Move notes from `## Unreleased` into a new section header with a vibe-coding name:

   ```markdown
   ## v3.0.1 — ✨ Your funny name
   ```

   Use an em dash (`—`) between the tag and the name. Emojis in the name are fine. CI fails if the current `Info.lua` version has no name. A short joke blurb under the header becomes the GitHub Release description lead-in.

2. Use the same tag string the workflow will publish (`v` + semver from `Info.lua`).

3. Leave `## Unreleased` in place for the next cycle (it can be empty).

Older changelog sections may omit a name or use the legacy `v1.0.0-rN` format; keep them as historical archive.

CI fails if the matching changelog section is missing.

## Release checklist

1. Land user-facing changes on `master` / `main`.
2. Bump `VERSION` in `Info.lua`:
   - Increment `revision` for patch releases.
   - Bump `minor` or `major` when behavior warrants it.
   - Keep `build = 0`.
3. Add the matching `## vX.Y.Z — Codename` section to `CHANGELOG.md`.
4. Push. Then run [`.github/workflows/release-plugin.yml`](.github/workflows/release-plugin.yml) manually. The workflow will:
   - ensure the tag does not already exist
   - extract the changelog section and codename
   - build the plugin zip
   - publish the GitHub Release titled `{codename} · vX.Y.Z`

To cut a release without other code changes, bump `Info.lua` and update `CHANGELOG.md`, push, then run the workflow.

## Local validation

```bash
chmod +x ./scripts/build_release_notes.sh ./scripts/parse_plugin_version.sh

# Version + required vibe-coding name for Info.lua
./scripts/parse_plugin_version.sh

# Validate parsing + changelog section for the version in Info.lua
./scripts/build_release_notes.sh --check-only

# Dry-run release notes for an existing historical tag (name not required)
./scripts/build_release_notes.sh --check-only --tag v2.0.0

# Dry-run release notes for an existing historical tag
./scripts/build_release_notes.sh --dry-run --tag v2.0.0 --commit "$(git rev-parse HEAD)" --run-id local
cat RELEASE_NOTES.md
```

## Install instructions for users

Published releases include platform, Lightroom requirement, install steps, commit link, workflow run link, and a link to the full changelog.
