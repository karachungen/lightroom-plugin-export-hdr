# Releasing Ultra HDR Export

Public builds are tagged `vX.Y.Z-rN` and published as GitHub Releases with a zip of `ExportHDR.lrplugin`.

## Version source of truth

[`ExportHDR.lrplugin/Info.lua`](ExportHDR.lrplugin/Info.lua):

```lua
VERSION = { major = 1, minor = 0, revision = 0, build = 8 }
```

| Field | Meaning |
|-------|---------|
| `major` / `minor` / `revision` | User-facing semver shown in release titles and Lightroom Plug-in Manager |
| `build` | Monotonic build number used in the Git tag suffix (`-r8`) |

The GitHub Actions workflow run number is metadata only. It is **not** the public build id.

## Changelog

Maintain [`CHANGELOG.md`](CHANGELOG.md) using [Keep a Changelog](https://keepachangelog.com/) sections.

Before each release:

1. Move notes from `## Unreleased` into a new exact section header:

   ```markdown
   ## v1.0.0-r8
   ```

2. Use the same tag string the workflow will publish (`v` + semver + `-r` + `VERSION.build`).

3. Leave `## Unreleased` in place for the next cycle (it can be empty).

CI fails if the matching changelog section is missing.

## Release checklist

1. Land user-facing changes on `master` / `main`.
2. Bump `VERSION` in `Info.lua`:
   - Increment `build` for every published build.
   - Bump `revision` (or `minor` / `major`) when behavior warrants a new semver.
3. Add the matching `## vX.Y.Z-rN` section to `CHANGELOG.md`.
4. Push. When relevant paths change, [`.github/workflows/release-plugin.yml`](.github/workflows/release-plugin.yml) will:
   - validate `build > 0`
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
./scripts/build_release_notes.sh --check-only --tag v1.0.0-r7

# Dry-run release notes for an existing historical tag
./scripts/build_release_notes.sh --dry-run --tag v1.0.0-r7 --commit "$(git rev-parse HEAD)" --run-id local
cat RELEASE_NOTES.md
```

## Install instructions for users

Published releases include platform, Lightroom requirement, install steps, commit link, workflow run link, and a link to the full changelog.
