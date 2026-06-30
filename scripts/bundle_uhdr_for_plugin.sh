#!/usr/bin/env bash
# Backward-compatible alias: build + bundle (no test/package).
# Prefer: ./scripts/build_plugin.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$SCRIPT_DIR/build_plugin.sh" build "$@"
"$SCRIPT_DIR/build_plugin.sh" bundle
