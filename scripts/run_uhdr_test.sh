#!/usr/bin/env bash
# Smoke-test uhdr_repack: encode defaults + --inspect (gain map size vs dimensions, primary XMP).
# Optional slice pass: original Ultra HDR + numbered 1x1 / 4x5 slices with per-file gain maps.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_DIR="$REPO_ROOT/test"
HDR="$TEST_DIR/hdr-raw.tif"
BASE="$TEST_DIR/sdr.jpg"
OUT="$TEST_DIR/out_uhdr.jpg"

PYTHON_BIN="$(command -v python3 || command -v python || command -v py.exe || true)"
if [[ -z "$PYTHON_BIN" ]]; then
	echo "Python 3 is required for the independent JPEG marker-order check." >&2
	exit 2
fi
PYTHON_ARGS=()
if [[ "$(basename "$PYTHON_BIN")" == "py.exe" ]]; then
	PYTHON_ARGS=(-3)
fi

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
}

echo "==> Using $BIN"
rm -f "$OUT" "$TEST_DIR"/out_uhdr_*.jpg
"$BIN" --hdr-tiff "$HDR" --base "$BASE" --out "$OUT"
assert_inspect_ok "$OUT"
"$PYTHON_BIN" "${PYTHON_ARGS[@]}" "$SCRIPT_DIR/check_jpeg_marker_order.py" --check "$OUT"
AUTO_INSPECT="$("$BIN" --inspect "$OUT")"
if echo "$AUTO_INSPECT" | grep -q 'min_boost=(1,1,1)'; then
	echo "FAIL: auto mode retained the old manual min boost of 1" >&2
	exit 11
fi
echo "OK: default encode — gain map matches dimensions and primary_xmp is present."

EXPLICIT_EXISTING_OUT="$TEST_DIR/out_existing_explicit_uhdr.jpg"
rm -f "$EXPLICIT_EXISTING_OUT"
"$BIN" --hdr-tiff "$HDR" --base "$BASE" --out "$EXPLICIT_EXISTING_OUT" \
	--gainmap-algorithm libultrahdr
if ! cmp -s "$OUT" "$EXPLICIT_EXISTING_OUT"; then
	echo "FAIL: explicit libultrahdr mode differs from the historical default path" >&2
	exit 14
fi
echo "OK: existing/libultrahdr mode is byte-identical to the historical default invocation."

COMPAT_OUT="$TEST_DIR/out_compatibility_scalar_uhdr.jpg"
COMPAT_MAP="$TEST_DIR/out_compatibility_scalar_gainmap.jpg"
rm -f "$COMPAT_OUT" "$COMPAT_MAP"
COMPAT_LOG="$("$BIN" --hdr-tiff "$HDR" --base "$BASE" --out "$COMPAT_OUT" \
	--gainmap-algorithm compatibility-scalar --gainmap-scale 4 --gainmap-quality 73 \
	--target-display-peak 1000 --gainmap-debug-out "$COMPAT_MAP")"
echo "$COMPAT_LOG"
COMPAT_INSPECT="$("$BIN" --inspect "$COMPAT_OUT")"
echo "$COMPAT_INSPECT"
echo "$COMPAT_INSPECT" | grep -q '^is_ultra_hdr: yes$'
echo "$COMPAT_INSPECT" | grep -q '^gainmap_size: 250x250$'
echo "$COMPAT_INSPECT" | grep -q '^gainmap_components: 1$'
echo "$COMPAT_INSPECT" | grep -q '^gainmap_gamma: 1$'
echo "$COMPAT_INSPECT" | grep -q '^gainmap_offsets: 0.000976562,0.000976562$'
echo "$COMPAT_INSPECT" | grep -Eq '^markers: .*primary_xmp=(yes|1).*iso_app2_hint=(yes|1)'
if echo "$COMPAT_LOG$COMPAT_INSPECT" | grep -Eqi '(^|[^[:alpha:]])(nan|inf)([^[:alpha:]]|$)'; then
	echo "FAIL: compatibility scalar produced NaN/Inf" >&2
	exit 15
fi
CALC_MIN="$(echo "$COMPAT_LOG" | sed -n 's/^Compatibility GainMapMin: //p')"
CALC_MAX="$(echo "$COMPAT_LOG" | sed -n 's/^Compatibility GainMapMax: //p')"
META_MIN="$(echo "$COMPAT_INSPECT" | sed -n 's/^gainmap_min_log2: //p')"
META_MAX="$(echo "$COMPAT_INSPECT" | sed -n 's/^gainmap_max_log2: //p')"
if [[ -z "$CALC_MIN" || "$CALC_MIN" != "$META_MIN" || "$CALC_MAX" != "$META_MAX" ]]; then
	echo "FAIL: compatibility metadata does not match calculated min/max" >&2
	exit 16
fi
if [[ "${CALC_MIN:0:1}" != "-" ]]; then
	echo "FAIL: fixture should exercise a negative compatibility GainMapMin" >&2
	exit 17
fi
echo "OK: compatibility scalar is grayscale, scaled, finite, negative-safe, and metadata-consistent."

MANUAL_OUT="$TEST_DIR/out_manual_boost_uhdr.jpg"
rm -f "$MANUAL_OUT"
"$BIN" --hdr-tiff "$HDR" --base "$BASE" --out "$MANUAL_OUT" \
	--min-content-boost 0.5 --max-content-boost 8
assert_inspect_ok "$MANUAL_OUT"
MANUAL_INSPECT="$("$BIN" --inspect "$MANUAL_OUT")"
echo "$MANUAL_INSPECT" | grep -q 'min_boost=(0.5,0.5,0.5)'
echo "$MANUAL_INSPECT" | grep -q 'max_boost=(8,8,8)'
echo "OK: manual content boost pair encodes successfully."

PAIR_ERROR_LOG="$TEST_DIR/out_content_boost_pair_error.log"
if "$BIN" --hdr-tiff "$HDR" --base "$BASE" --out "$TEST_DIR/out_invalid_boost.jpg" \
	--min-content-boost 0.5 >"$PAIR_ERROR_LOG" 2>&1; then
	echo "FAIL: only --min-content-boost unexpectedly succeeded" >&2
	exit 12
fi
grep -q "Both --min-content-boost and --max-content-boost must be specified together" "$PAIR_ERROR_LOG"
if "$BIN" --hdr-tiff "$HDR" --base "$BASE" --out "$TEST_DIR/out_invalid_boost.jpg" \
	--max-content-boost 8 >"$PAIR_ERROR_LOG" 2>&1; then
	echo "FAIL: only --max-content-boost unexpectedly succeeded" >&2
	exit 13
fi
grep -q "Both --min-content-boost and --max-content-boost must be specified together" "$PAIR_ERROR_LOG"
rm -f "$PAIR_ERROR_LOG" "$TEST_DIR/out_invalid_boost.jpg"
echo "OK: unpaired content boost flags are rejected."

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
echo "==> Slice test (1x1 + 4x5, SDR copy preserved like Lightroom plug-in)"
"$BIN" --hdr-tiff "$HDR" --base "$SDR_COPY" --out "$SLICE_OUT" --slice-aspect 1x1
assert_inspect_ok "$SLICE_OUT"

shopt -s nullglob
slices_1x1=("$TEST_DIR"/out_slice_uhdr_1x1_*.jpg)
if [[ ${#slices_1x1[@]} -lt 1 ]]; then
	echo "FAIL: expected at least one 1x1 slice next to $SLICE_OUT" >&2
	exit 8
fi
for p in "${slices_1x1[@]}"; do
	echo "==> inspect slice $p"
	assert_inspect_ok "$p"
done

rm -f "$SLICE_OUT" "$TEST_DIR"/out_slice_uhdr_*.jpg
"$BIN" --hdr-tiff "$HDR" --base "$SDR_COPY" --out "$SLICE_OUT" --slice-aspect 4x5
assert_inspect_ok "$SLICE_OUT"

slices_4x5=("$TEST_DIR"/out_slice_uhdr_4x5_*.jpg)
if [[ ${#slices_4x5[@]} -lt 1 ]]; then
	echo "FAIL: expected at least one 4x5 slice next to $SLICE_OUT" >&2
	exit 9
fi
for p in "${slices_4x5[@]}"; do
	echo "==> inspect slice $p"
	assert_inspect_ok "$p"
	# 4:5 full-height: width should be floor_to_even(H * 4/5) of slice height.
	local_h="$(echo "$("$BIN" --inspect "$p")" | sed -n 's/^dimensions: \([0-9]*\)x\([0-9]*\)/\2/p')"
	local_w="$(echo "$("$BIN" --inspect "$p")" | sed -n 's/^dimensions: \([0-9]*\)x\([0-9]*\)/\1/p')"
	expected_w=$(( (local_h * 4 / 5) / 2 * 2 ))
	if [[ "$local_w" -ne "$expected_w" ]]; then
		echo "FAIL: 4x5 slice width $local_w != expected even floor(H*4/5)=$expected_w" >&2
		exit 10
	fi
done

echo "OK: slice encode — original + numbered slices are valid Ultra HDR with gain maps."
