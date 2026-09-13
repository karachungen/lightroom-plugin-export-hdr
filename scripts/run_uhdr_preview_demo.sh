#!/usr/bin/env bash
# Launch uhdr_repack --edit with bundled preview session (requires bundle + fixtures).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SESSION="$REPO_ROOT/test/ui/preview_session.json"
FIXTURE="$REPO_ROOT/test/ui/fixtures/DSC02993.tif"

if [[ ! -f "$FIXTURE" ]]; then
  echo "Missing $FIXTURE — run: ./scripts/build_plugin.sh bundle (copies fixtures first)" >&2
  exit 1
fi

supports_edit() {
  local bin="$1"
  { "$bin" 2>&1 || true; } | grep -q -- '--edit'
}

BIN=""
for c in \
  "$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack" \
  "$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack"; do
  if [[ -x "$c" ]] && supports_edit "$c"; then
    BIN="$c"
    break
  fi
done

if [[ -z "$BIN" ]]; then
  echo "uhdr_repack not built. Run ./scripts/build_plugin.sh build" >&2
  exit 1
fi

cd "$REPO_ROOT"
if [[ "${UHDR_DEMO_KEEP_EDITS:-0}" != "1" ]]; then
  rm -f \
    "$REPO_ROOT/test/ui/preview_out/dsc02993.gainmap" \
    "$REPO_ROOT/test/ui/preview_out/dji_pano.gainmap" \
    "$REPO_ROOT/test/ui/preview_out/legacy_1000.gainmap"
fi
exec "$BIN" --edit --session "$SESSION"
