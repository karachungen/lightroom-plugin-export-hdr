#!/usr/bin/env bash
# Parse Info.lua version, validate changelog section, write RELEASE_NOTES.md for GitHub Releases.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
info_lua="${repo_root}/ExportHDR.lrplugin/Info.lua"
changelog="${repo_root}/CHANGELOG.md"
out_file="${repo_root}/RELEASE_NOTES.md"

usage() {
  cat <<'EOF'
Usage: build_release_notes.sh [--check-only] [--dry-run] [--tag TAG] [--commit SHA] [--run-id ID] [--repository OWNER/REPO]

  --check-only   Validate version/changelog/tag; do not write RELEASE_NOTES.md
  --dry-run      Write RELEASE_NOTES.md without requiring a new tag or build > 0
  --tag TAG      Override tag (default: parsed from Info.lua)
  --commit SHA   Commit SHA for release footer (default: GITHUB_SHA or HEAD)
  --run-id ID    Workflow run id for release footer (default: GITHUB_RUN_ID or "local")
  --repository   GitHub repository slug (default: GITHUB_REPOSITORY or karachungen/lightroom-plugin-export-hdr)
EOF
}

check_only=0
dry_run=0
override_tag=""
commit_sha="${GITHUB_SHA:-$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || echo "unknown")}"
run_id="${GITHUB_RUN_ID:-local}"
repository="${GITHUB_REPOSITORY:-karachungen/lightroom-plugin-export-hdr}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --check-only) check_only=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    --tag) override_tag="$2"; shift 2 ;;
    --commit) commit_sha="$2"; shift 2 ;;
    --run-id) run_id="$2"; shift 2 ;;
    --repository) repository="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ ! -f "$info_lua" ]]; then
  echo "Missing Info.lua: $info_lua" >&2
  exit 1
fi

if [[ ! -f "$changelog" ]]; then
  echo "Missing CHANGELOG.md: $changelog" >&2
  exit 1
fi

line=$(grep 'VERSION = {' "$info_lua")
major=$(echo "$line" | sed -E 's/.*major *= *([0-9]+).*/\1/')
minor=$(echo "$line" | sed -E 's/.*minor *= *([0-9]+).*/\1/')
revision=$(echo "$line" | sed -E 's/.*revision *= *([0-9]+).*/\1/')
build=$(echo "$line" | sed -E 's/.*build *= *([0-9]+).*/\1/')

if [[ -z "$major" || -z "$minor" || -z "$revision" || -z "$build" ]]; then
  echo "Failed to parse VERSION from Info.lua" >&2
  exit 1
fi

if [[ "$build" -eq 0 && "$dry_run" -eq 0 && -z "$override_tag" ]]; then
  echo "VERSION.build must be > 0 before publishing a release (currently 0 in Info.lua)." >&2
  exit 1
fi

semver="${major}.${minor}.${revision}"
tag="${override_tag:-v${semver}-r${build}}"

if [[ "$dry_run" -eq 0 && -z "$override_tag" ]] && git -C "$repo_root" rev-parse "$tag" >/dev/null 2>&1; then
  echo "Tag already exists: $tag" >&2
  exit 1
fi

section_header="## ${tag}"
changelog_section=$(
  awk -v header="$section_header" '
    $0 == header { found = 1; next }
    found && /^## / { exit }
    found { print }
  ' "$changelog"
)

if [[ -z "${changelog_section//[[:space:]]/}" ]]; then
  echo "CHANGELOG.md is missing section: ${section_header}" >&2
  exit 1
fi

if [[ "$check_only" -eq 1 ]]; then
  echo "OK: version ${semver} build ${build}, tag ${tag}, changelog section present"
  exit 0
fi

{
  printf '%s\n\n' "$changelog_section"
  cat <<EOF
---

**Platform:** **macOS 26 (Tahoe), ARM64.** Built on GitHub \`macos-26\`.

**Lightroom:** Classic **14+** (\`LrSdkMinimumVersion\` in \`Info.lua\`).

**Install:** unzip the archive, then in Lightroom **File → Plug-in Manager → Add** and select the \`ExportHDR.lrplugin\` folder.

**Commit:** [\`${commit_sha}\`](https://github.com/${repository}/commit/${commit_sha})  
**Workflow run:** [${run_id}](https://github.com/${repository}/actions/runs/${run_id})

**Full changelog:** [CHANGELOG.md](https://github.com/${repository}/blob/${tag}/CHANGELOG.md)
EOF
} > "$out_file"

echo "Wrote ${out_file} for tag ${tag}"
