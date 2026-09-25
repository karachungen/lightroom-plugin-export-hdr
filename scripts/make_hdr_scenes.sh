#!/usr/bin/env bash
# Generate committed Ultra HDR upload fixtures for the sunset/neon/pastel HDR scenes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack"
OUT="${1:-$REPO_ROOT/test/hdr-scenes}"

if [[ ! -x "$BIN" ]]; then
	echo "Build uhdr_repack first: CMAKE_PREFIX_PATH=/opt/homebrew/opt/qt ./scripts/build_plugin.sh build" >&2
	exit 1
fi

mkdir -p "$OUT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

"$BIN" --write-hdr-scenes "$WORK"

for s in sunset neon pastel; do
	"$BIN" --hdr-tiff "$WORK/$s-hdr.tif" --base "$WORK/$s-sdr.jpg" --out "$OUT/$s-uhdr.jpg"
	"$BIN" --verify-uhdr "$OUT/$s-uhdr.jpg" --expect-size 1080x1350 --max-bytes 8388608
	echo "$OUT/$s-uhdr.jpg"
done
