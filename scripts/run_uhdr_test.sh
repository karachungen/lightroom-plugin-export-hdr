#!/usr/bin/env bash
# Smoke-test uhdr_repack: encode defaults + --inspect (gain map size vs dimensions, primary XMP).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_DIR="$REPO_ROOT/test"
HDR="$TEST_DIR/hdr-raw.tif"
BASE="$TEST_DIR/sdr.jpg"
OUT="$TEST_DIR/out_uhdr.jpg"

BIN=""
if [[ -x "$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack" ]]; then
	BIN="$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack"
elif [[ -x "$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack" ]]; then
	BIN="$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack"
else
	echo "uhdr_repack not found. Build with:" >&2
	echo "  cmake -S tools/uhdr_repack -B tools/uhdr_repack/build -DCMAKE_BUILD_TYPE=Release && cmake --build tools/uhdr_repack/build" >&2
	echo "or: ./scripts/bundle_uhdr_for_plugin.sh" >&2
	exit 2
fi

if [[ ! -f "$HDR" ]] || [[ ! -f "$BASE" ]]; then
	echo "Missing test inputs. See test/README.md — need:" >&2
	echo "  $HDR" >&2
	echo "  $BASE" >&2
	exit 3
fi

echo "==> Using $BIN"
rm -f "$OUT"
"$BIN" --hdr-tiff "$HDR" --base "$BASE" --out "$OUT"

INSPECT="$("$BIN" --inspect "$OUT")"
echo "$INSPECT"

dims="$(echo "$INSPECT" | sed -n 's/^dimensions: \([0-9]*\)x\([0-9]*\)/\1x\2/p')"
gm="$(echo "$INSPECT" | sed -n 's/^gainmap_size: \([0-9]*\)x\([0-9]*\)/\1x\2/p')"
xmp_line="$(echo "$INSPECT" | grep '^markers:' || true)"

if [[ -z "$dims" ]] || [[ -z "$gm" ]]; then
	echo "inspect: could not parse dimensions / gainmap_size" >&2
	exit 4
fi

if [[ "$dims" != "$gm" ]]; then
	echo "FAIL: gainmap_size ($gm) != dimensions ($dims) — expected match at default --gainmap-scale 1" >&2
	exit 5
fi

if ! echo "$xmp_line" | grep -Eq 'primary_xmp=(yes|1)'; then
	echo "FAIL: expected primary_xmp=yes or primary_xmp=1 (vendored libultrahdr with UHDR_WRITE_XMP)" >&2
	exit 6
fi

echo "OK: gain map matches dimensions and primary_xmp is present."
