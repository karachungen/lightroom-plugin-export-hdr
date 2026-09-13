#!/usr/bin/env bash
# Verify gain map generation, edit round-trip, and encode with --gainmap-in.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack"
SDR="$REPO_ROOT/test/ui/fixtures/DSC02993.jpg"
HDR="$REPO_ROOT/test/ui/fixtures/DSC02993.tif"
WORK="$REPO_ROOT/test/ui/preview_out/gainmap_test"
GAIN="$WORK/auto.gainmap"
EDITED="$WORK/edited.gainmap"
OUT="$WORK/edited_uhdr.jpg"

if [[ ! -x "$BIN" ]]; then
	echo "Build uhdr_repack first: CMAKE_PREFIX_PATH=/opt/homebrew/opt/qt ./scripts/build_plugin.sh build" >&2
	exit 1
fi
if [[ ! -f "$SDR" || ! -f "$HDR" ]]; then
	echo "Missing fixtures — run: ./scripts/copy_ui_fixtures.sh" >&2
	exit 1
fi

mkdir -p "$WORK"
rm -f "$GAIN" "$EDITED" "$OUT"

echo "==> Compute auto gain map"
DUMP="$("$BIN" --dump-gainmap --sdr "$SDR" --hdr-tiff "$HDR" --out "$GAIN")"
echo "$DUMP"

python3 - <<'PY' "$GAIN"
import struct, sys
path = sys.argv[1]
with open(path, "rb") as f:
    w, h = struct.unpack("ii", f.read(8))
    data = struct.unpack(f"{w*h}f", f.read(w*h*4))
mn, mx = min(data), max(data)
spread = mx - mn
print(f"gainmap pixels={w}x{h} min={mn:.3f} max={mx:.3f} spread={spread:.3f}")
if spread < 0.05:
    raise SystemExit("FAIL: gain map is nearly flat (expected HDR variation)")
if mn < 0.99:
    raise SystemExit(f"FAIL: unexpected min gain {mn}")
print("OK: gain map has HDR variation")
PY

echo "==> Simulate brush edit (boost center region)"
python3 - <<'PY' "$GAIN" "$EDITED"
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    w, h = struct.unpack("ii", f.read(8))
    gain = list(struct.unpack(f"{w*h}f", f.read(w*h*4)))
cx, cy = w // 2, h // 2
radius = min(w, h) // 8
changed = 0
for y in range(h):
    for x in range(w):
        if (x-cx)**2 + (y-cy)**2 <= radius*radius:
            i = y*w+x
            if gain[i] < 50:
                gain[i] = min(500.0, gain[i] * 3.0)
                changed += 1
with open(dst, "wb") as f:
    f.write(struct.pack("ii", w, h))
    f.write(struct.pack(f"{w*h}f", *gain))
print(f"edited {changed} pixels in center")
if changed == 0:
    raise SystemExit("FAIL: no pixels edited")
print("OK: edited gain map written")
PY

echo "==> Encode with edited gain map"
"$BIN" --hdr-tiff "$HDR" --base "$SDR" --out "$OUT" --gainmap-in "$EDITED" >/dev/null

if [[ ! -f "$OUT" ]]; then
	echo "FAIL: encoder did not write $OUT" >&2
	exit 1
fi

INSPECT="$("$BIN" --inspect "$OUT")"
echo "$INSPECT" | head -8
echo "$INSPECT" | grep -q "is_ultra_hdr: yes" || {
	echo "FAIL: output is not Ultra HDR" >&2
	exit 1
}

echo "OK: gain map pipeline (compute → edit → encode)"
