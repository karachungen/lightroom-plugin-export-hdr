#!/usr/bin/env bash
# Parse ExportHDR.lrplugin/Info.lua version fields.
# Usage:
#   ./scripts/parse_plugin_version.sh              # KEY=value lines to stdout
#   ./scripts/parse_plugin_version.sh --github-output  # append to GITHUB_OUTPUT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
INFO_LUA="$REPO_ROOT/ExportHDR.lrplugin/Info.lua"

usage() {
	cat <<'EOF'
Usage: parse_plugin_version.sh [--github-output]

  --github-output  Write major, minor, revision, semver, tag to GITHUB_OUTPUT
EOF
}

github_output=0
while [[ $# -gt 0 ]]; do
	case "$1" in
	--github-output) github_output=1; shift ;;
	-h | --help) usage; exit 0 ;;
	*) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
done

if [[ ! -f "$INFO_LUA" ]]; then
	echo "Missing Info.lua: $INFO_LUA" >&2
	exit 1
fi

line=$(grep 'VERSION = {' "$INFO_LUA")
major=$(echo "$line" | sed -E 's/.*major *= *([0-9]+).*/\1/')
minor=$(echo "$line" | sed -E 's/.*minor *= *([0-9]+).*/\1/')
revision=$(echo "$line" | sed -E 's/.*revision *= *([0-9]+).*/\1/')
build=$(echo "$line" | sed -E 's/.*build *= *([0-9]+).*/\1/')

if [[ -z "$major" || -z "$minor" || -z "$revision" || -z "$build" ]]; then
	echo "Failed to parse VERSION from Info.lua" >&2
	exit 1
fi

semver="${major}.${minor}.${revision}"
tag="v${semver}-r${build}"

emit() {
	local key="$1"
	local value="$2"
	if [[ "$github_output" -eq 1 ]]; then
		echo "${key}=${value}" >>"${GITHUB_OUTPUT:?GITHUB_OUTPUT is not set}"
	else
		echo "${key}=${value}"
	fi
}

emit major "$major"
emit minor "$minor"
emit revision "$revision"
emit build "$build"
emit semver "$semver"
emit tag "$tag"
