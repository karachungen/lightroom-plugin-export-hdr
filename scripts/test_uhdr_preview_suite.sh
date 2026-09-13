#!/usr/bin/env bash
# Full preview/HDR test suite — headless + optional UI screenshot.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack"
SESSION="$REPO_ROOT/test/ui/preview_session.json"
SCREENSHOT="${UHDR_SCREENSHOT_OUT:-$REPO_ROOT/test/ui/preview_out/preview_screenshot.png}"

find_bin() {
	if [[ -x "$BIN" ]]; then
		return 0
	fi
	if [[ -x "$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack" ]]; then
		BIN="$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack"
		return 0
	fi
	return 1
}

echo "==> [1/6] Smoke encode + inspect (existing CI test)"
"$SCRIPT_DIR/run_uhdr_test.sh"

echo ""
echo "==> [2/6] Gain map compute / edit / encode"
"$SCRIPT_DIR/test_gainmap_preview.sh"

echo ""
echo "==> [3/6] CLI --dump-gainmap on DJI pano fixture"
DJI_SDR="$REPO_ROOT/test/ui/fixtures/DJI_0001-20260316-184709-Pano.jpg"
DJI_HDR="$REPO_ROOT/test/ui/fixtures/DJI_0001-20260316-184709-Pano.tif"
DJI_GAIN="$REPO_ROOT/test/ui/preview_out/dji_auto.gainmap"
if [[ -f "$DJI_SDR" && -f "$DJI_HDR" ]]; then
	"$BIN" --dump-gainmap --sdr "$DJI_SDR" --hdr-tiff "$DJI_HDR" --out "$DJI_GAIN"
	python3 - <<'PY' "$DJI_GAIN"
import struct, sys
with open(sys.argv[1], "rb") as f:
    w, h = struct.unpack("ii", f.read(8))
    data = struct.unpack(f"{w*h}f", f.read(w*h*4))
if max(data) - min(data) < 0.05:
    raise SystemExit("FAIL: DJI gain map flat")
print(f"OK DJI gain map {w}x{h} spread={max(data)-min(data):.3f}")
PY
else
	echo "SKIP DJI fixtures missing"
fi

echo ""
echo "==> [4/6] Session JSON load paths"
python3 - <<'PY' "$SESSION" "$REPO_ROOT"
import json, os, sys
session_path, repo = sys.argv[1], sys.argv[2]
with open(session_path) as f:
    data = json.load(f)
for item in data["items"]:
    for key in ("sdr", "hdr_tiff", "out"):
        p = item[key]
        if not os.path.isabs(p):
            p = os.path.join(repo, p)
        if key != "out" and not os.path.isfile(p):
            raise SystemExit(f"FAIL missing {key} for {item['id']}: {p}")
print(f"OK session items={len(data['items'])} paths resolve")
PY

echo ""
echo "==> [5/6] Qt offscreen --self-test (gainmap, brush, eraser, smooth, HDR composite, apply batch)"
if ! find_bin; then
	echo "FAIL: uhdr_repack binary not found" >&2
	exit 1
fi
if ! { "$BIN" 2>&1 || true; } | grep -q -- '--self-test'; then
	echo "SKIP --self-test (rebuild with UHDR_ENABLE_GUI=ON)"
else
	(
		cd "$REPO_ROOT"
		QT_QPA_PLATFORM=offscreen "$BIN" --self-test --session "$SESSION"
	)
fi

echo ""
echo "==> [6/6] Optional UI screenshot"
if [[ "${UHDR_SKIP_SCREENSHOT:-}" == "1" ]]; then
	echo "SKIP screenshot (UHDR_SKIP_SCREENSHOT=1)"
else
	if [[ -f "$SCRIPT_DIR/capture_preview_screenshot.sh" ]]; then
		UHDR_SCREENSHOT_OUT="$SCREENSHOT" "$SCRIPT_DIR/capture_preview_screenshot.sh" || {
			echo "WARN: screenshot capture failed (non-fatal)"
		}
	else
		echo "SKIP screenshot script missing"
	fi
fi

echo ""
echo "ALL_PREVIEW_TESTS_OK"
