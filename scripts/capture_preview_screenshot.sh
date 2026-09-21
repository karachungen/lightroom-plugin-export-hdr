#!/usr/bin/env bash
# Launch preview GUI briefly and capture a window screenshot (macOS).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="${UHDR_SCREENSHOT_OUT:-$REPO_ROOT/test/ui/preview_out/preview_screenshot.png}"
SESSION="$REPO_ROOT/test/ui/preview_session.json"

supports_edit() {
	local bin="$1"
	{ "$bin" 2>&1 || true; } | grep -q -- '--edit'
}

BIN=""
for c in "$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack" \
	"$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack"; do
	if [[ -x "$c" ]] && supports_edit "$c"; then
		BIN="$c"
		break
	fi
done

if [[ -z "$BIN" ]]; then
	echo "No GUI binary for screenshot" >&2
	exit 1
fi

mkdir -p "$(dirname "$OUT")"
cd "$REPO_ROOT"
rm -f \
	"$REPO_ROOT/test/ui/preview_out/dsc02993.gainmap" \
	"$REPO_ROOT/test/ui/preview_out/dji_pano.gainmap" \
	"$REPO_ROOT/test/ui/preview_out/legacy_1000.gainmap"

"$BIN" --edit --session "$SESSION" &
APP_PID=$!

cleanup() {
	kill "$APP_PID" 2>/dev/null || true
	wait "$APP_PID" 2>/dev/null || true
}
trap cleanup EXIT

sleep 4
osascript - "$APP_PID" <<'APPLESCRIPT' 2>/dev/null || true
on run argv
	tell application "System Events"
		set targetPid to (item 1 of argv) as integer
		set frontmost of first application process whose unix id is targetPid to true
	end tell
end run
APPLESCRIPT
sleep 1

WIN_ID=""
for _ in 1 2 3 4 5; do
	WIN_ID="$(APP_PID="$APP_PID" python3 - <<'PY' 2>/dev/null || true
import os
import Quartz
pid = int(os.environ["APP_PID"])
opts = Quartz.kCGWindowListOptionOnScreenOnly | Quartz.kCGWindowListExcludeDesktopElements
for w in Quartz.CGWindowListCopyWindowInfo(opts, Quartz.kCGNullWindowID):
    if int(w.get("kCGWindowOwnerPID", -1)) == pid and w.get("kCGWindowLayer", 1) == 0:
        print(w["kCGWindowNumber"])
        break
PY
)"
	if [[ -n "$WIN_ID" ]]; then
		break
	fi
	sleep 1
done

if [[ -n "$WIN_ID" ]]; then
	screencapture -x -l "$WIN_ID" "$OUT"
	echo "OK screenshot window $WIN_ID -> $OUT"
else
	screencapture -x "$OUT"
	echo "OK screenshot (full screen fallback) -> $OUT"
fi

if [[ ! -s "$OUT" ]]; then
	echo "FAIL: screenshot empty" >&2
	exit 1
fi

python3 - <<'PY' "$OUT"
import sys
from pathlib import Path
p = Path(sys.argv[1])
if p.stat().st_size < 5000:
    raise SystemExit("FAIL: screenshot too small")
print(f"OK screenshot size={p.stat().st_size} bytes")
PY
