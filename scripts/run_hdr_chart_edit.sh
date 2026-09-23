#!/usr/bin/env bash
# Open the Ultra HDR editor on the committed stop/color chart.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HDR="$REPO_ROOT/test/hdr-chart/hdr-chart.tif"
SDR="$REPO_ROOT/test/hdr-chart/sdr-chart.jpg"
SESSION="$REPO_ROOT/test/hdr-chart/session.json"

if [[ ! -f "$HDR" || ! -f "$SDR" ]]; then
  echo "Missing chart pair. Generate with: uhdr_repack --write-hdr-chart test/hdr-chart" >&2
  exit 1
fi

has_edit_gui() {
  local bin="$1"
  ! { "$bin" --edit 2>&1 || true; } | grep -q "without GUI support"
}

BIN=""
for c in \
  "$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack" \
  "$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack.exe" \
  "$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack" \
  "$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack.exe"; do
  if [[ -x "$c" || -f "$c" ]] && has_edit_gui "$c"; then
    BIN="$c"
    break
  fi
done

if [[ -z "$BIN" ]]; then
  echo "uhdr_repack with --edit was not found. Build the GUI binary (UHDR_ENABLE_GUI must stay on)." >&2
  exit 1
fi

cd "$REPO_ROOT"
echo "Opening Ultra HDR editor with the stop/color chart"
echo "  $BIN"
exec "$BIN" --edit --session "$SESSION"
