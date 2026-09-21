#!/usr/bin/env bash
# Smoke-test uhdr_repack: encode defaults + --inspect (gain map size vs dimensions, primary XMP).
# Optional slice pass: Instagram crop to --out (one slide by default).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_DIR="$REPO_ROOT/test"
HDR="$TEST_DIR/hdr-raw.tif"
BASE="$TEST_DIR/sdr.jpg"
OUT="$TEST_DIR/out_uhdr.jpg"

BIN=""
if [[ -x "$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack" ]]; then
	BIN="$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack"
elif [[ -x "$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack.exe" ]]; then
	BIN="$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack.exe"
elif [[ -x "$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack" ]]; then
	BIN="$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack"
elif [[ -x "$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack.exe" ]]; then
	BIN="$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack.exe"
elif [[ -x "$REPO_ROOT/tools/uhdr_repack/build/Release/uhdr_repack.exe" ]]; then
	BIN="$REPO_ROOT/tools/uhdr_repack/build/Release/uhdr_repack.exe"
else
	echo "uhdr_repack not found. Build with:" >&2
	echo "  ./scripts/build_plugin.sh" >&2
	echo "or: ./scripts/bundle_uhdr_for_plugin.sh (macOS) / .\\scripts\\build_plugin.ps1 (Windows)" >&2
	exit 2
fi

# Windows: bundled plugin binary has uhdr.dll next to exe.
if [[ "$BIN" == *.exe ]]; then
	BIN_DIR="$(dirname "$BIN")"
	export PATH="$BIN_DIR:$PATH"
fi

if [[ ! -f "$HDR" ]] || [[ ! -f "$BASE" ]]; then
	echo "Missing test inputs. See test/README.md — need:" >&2
	echo "  $HDR" >&2
	echo "  $BASE" >&2
	exit 3
fi

assert_inspect_ok() {
	local path="$1"
	local inspect
	inspect="$("$BIN" --inspect "$path")"
	echo "$inspect"

	local dims gm xmp_line
	dims="$(echo "$inspect" | sed -n 's/^dimensions: \([0-9]*\)x\([0-9]*\)/\1x\2/p')"
	gm="$(echo "$inspect" | sed -n 's/^gainmap_size: \([0-9]*\)x\([0-9]*\)/\1x\2/p')"
	xmp_line="$(echo "$inspect" | grep '^markers:' || true)"

	if [[ -z "$dims" ]] || [[ -z "$gm" ]]; then
		echo "inspect: could not parse dimensions / gainmap_size for $path" >&2
		exit 4
	fi

	if [[ "$dims" != "$gm" ]]; then
		echo "FAIL: gainmap_size ($gm) != dimensions ($dims) for $path" >&2
		exit 5
	fi

	if ! echo "$xmp_line" | grep -Eq 'primary_xmp=(yes|1)'; then
		echo "FAIL: expected primary_xmp=yes or primary_xmp=1 for $path" >&2
		exit 6
	fi

	if ! echo "$inspect" | grep -q '^is_ultra_hdr: yes'; then
		echo "FAIL: expected is_ultra_hdr: yes for $path" >&2
		exit 7
	fi

	if ! grep -a -F "https://hdr.karachun.by/" "$path" >/dev/null; then
		echo "FAIL: expected xmpRights WebStatement https://hdr.karachun.by/ in $path" >&2
		exit 11
	fi
	if ! grep -a -F "https://github.com/karachungen/lightroom-plugin-export-hdr" "$path" >/dev/null; then
		echo "FAIL: expected xmpRights UsageTerms GitHub URL in $path" >&2
		exit 11
	fi
}

echo "==> Using $BIN"
rm -f "$OUT" "$TEST_DIR"/out_uhdr_*.jpg
"$BIN" --hdr-tiff "$HDR" --base "$BASE" --out "$OUT"
assert_inspect_ok "$OUT"
echo "OK: default encode — gain map matches dimensions and primary_xmp is present."

CYR_DIR="$TEST_DIR/тест"
CYR_OUT="$CYR_DIR/out_uhdr.jpg"
mkdir -p "$CYR_DIR"
rm -f "$CYR_OUT"
echo "==> Cyrillic folder path test ($CYR_DIR)"
"$BIN" --hdr-tiff "$HDR" --base "$BASE" --out "$CYR_OUT"
assert_inspect_ok "$CYR_OUT"
echo "OK: Cyrillic folder encode — UTF-8 paths work."

SLICE_OUT="$TEST_DIR/out_slice_uhdr.jpg"
SDR_COPY="$(mktemp -t uhdr_sdr_copy.XXXXXX).jpg"
cp "$BASE" "$SDR_COPY"
trap 'rm -f "$SDR_COPY"' EXIT

rm -f "$SLICE_OUT" "$TEST_DIR"/out_slice_uhdr_*.jpg
echo "==> Slice test (1x1 + 4x5 single-slide Instagram crop, below 1× stays native)"
"$BIN" --hdr-tiff "$HDR" --base "$SDR_COPY" --out "$SLICE_OUT" --slice-aspect 1x1
assert_inspect_ok "$SLICE_OUT"
dims_1x1="$(echo "$("$BIN" --inspect "$SLICE_OUT")" | sed -n 's/^dimensions: \([0-9]*\)x\([0-9]*\)/\1x\2/p')"
if [[ "$dims_1x1" != "1000x1000" ]]; then
	echo "FAIL: 1x1 native crop was $dims_1x1, expected 1000x1000" >&2
	exit 8
fi
shopt -s nullglob
slices_1x1=("$TEST_DIR"/out_slice_uhdr_1x1_*.jpg)
if [[ ${#slices_1x1[@]} -ne 0 ]]; then
	echo "FAIL: default 1x1 crop should write only $SLICE_OUT, found numbered slices" >&2
	exit 8
fi

rm -f "$SLICE_OUT" "$TEST_DIR"/out_slice_uhdr_*.jpg
"$BIN" --hdr-tiff "$HDR" --base "$SDR_COPY" --out "$SLICE_OUT" --slice-aspect 4x5
assert_inspect_ok "$SLICE_OUT"
inspect_4x5="$("$BIN" --inspect "$SLICE_OUT")"
dims_4x5="$(echo "$inspect_4x5" | sed -n 's/^dimensions: \([0-9]*\)x\([0-9]*\)/\1x\2/p')"
if [[ "$dims_4x5" != "800x1000" ]]; then
	echo "FAIL: 4x5 native crop was $dims_4x5, expected 800x1000" >&2
	exit 9
fi
slices_4x5=("$TEST_DIR"/out_slice_uhdr_4x5_*.jpg)
if [[ ${#slices_4x5[@]} -ne 0 ]]; then
	echo "FAIL: default 4x5 crop should write only $SLICE_OUT, found numbered slices" >&2
	exit 9
fi

echo "OK: slice encode — crops below 1× stay native (no silent upscale)."

DSC="$REPO_ROOT/test/ui/fixtures/DSC02993.jpg"
DSC_HDR="$REPO_ROOT/test/ui/fixtures/DSC02993.tif"
if [[ -f "$DSC" && -f "$DSC_HDR" ]]; then
	FEED_OUT="$TEST_DIR/out_feed_1080.jpg"
	rm -f "$FEED_OUT"
	echo "==> Optional 4:5 --out-width 1080 on 1152x1440 fixture"
	"$BIN" --hdr-tiff "$DSC_HDR" --base "$DSC" --out "$FEED_OUT" --slice-aspect 4x5 --out-width 1080
	assert_inspect_ok "$FEED_OUT"
	dims_feed="$(echo "$("$BIN" --inspect "$FEED_OUT")" | sed -n 's/^dimensions: \([0-9]*\)x\([0-9]*\)/\1x\2/p')"
	if [[ "$dims_feed" != "1080x1350" ]]; then
		echo "FAIL: 4x5 --out-width 1080 was $dims_feed, expected 1080x1350" >&2
		exit 12
	fi
	echo "OK: 4:5 --out-width 1080 scales 1152x1440 to 1080x1350."
	SMART_OUT="$TEST_DIR/out_feed_smart.jpg"
	rm -f "$SMART_OUT"
	echo "==> Optional 4:5 smart pick (no --out-width) on 1152x1440 fixture"
	"$BIN" --hdr-tiff "$DSC_HDR" --base "$DSC" --out "$SMART_OUT" --slice-aspect 4x5
	assert_inspect_ok "$SMART_OUT"
	dims_smart="$(echo "$("$BIN" --inspect "$SMART_OUT")" | sed -n 's/^dimensions: \([0-9]*\)x\([0-9]*\)/\1x\2/p')"
	if [[ "$dims_smart" != "1152x1440" ]]; then
		echo "FAIL: 4x5 smart pick was $dims_smart, expected 1152x1440" >&2
		exit 13
	fi
	echo "OK: 4:5 smart pick keeps 1152x1440 (native, below 2×)."
fi
