#!/usr/bin/env bash
# Copy UI preview JPEG/TIFF pairs into test/ui/fixtures/ (build-time only).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST="$REPO_ROOT/test/ui/fixtures"

mkdir -p "$DEST"

required=(
  DSC02993.jpg
  DSC02993.tif
  DJI_0001-20260316-184709-Pano.jpg
  DJI_0001-20260316-184709-Pano.tif
)

dest_has_required() {
  local f
  for f in "${required[@]}"; do
    if [[ ! -f "$DEST/$f" ]]; then
      return 1
    fi
  done
  return 0
}

if [[ -z "${UHDR_FIXTURES_SRC:-}" ]]; then
  if dest_has_required; then
    echo "==> UHDR_FIXTURES_SRC unset; using existing files in $DEST"
    exit 0
  fi
  echo "copy_ui_fixtures: no source folder and missing files in $DEST." >&2
  echo "Set UHDR_FIXTURES_SRC to a folder containing UI preview JPEG/TIFF pairs." >&2
  exit 1
fi

SRC="$UHDR_FIXTURES_SRC"

missing=()
for f in "${required[@]}"; do
  if [[ ! -f "$SRC/$f" ]]; then
    missing+=("$f")
  fi
done

if [[ ${#missing[@]} -gt 0 ]]; then
  if dest_has_required; then
    echo "==> UI fixtures source missing; using existing files in $DEST"
    exit 0
  fi
  echo "copy_ui_fixtures: missing source files under $SRC:" >&2
  printf '  %s\n' "${missing[@]}" >&2
  echo "Set UHDR_FIXTURES_SRC to a folder containing UI preview JPEG/TIFF pairs." >&2
  exit 1
fi

for f in "${required[@]}"; do
  cp -f "$SRC/$f" "$DEST/"
done

echo "==> UI fixtures copied to $DEST"

if command -v sips &>/dev/null; then
  check_dim() {
    local file="$1" ew="$2" eh="$3"
    local w h
    w="$(sips -g pixelWidth "$file" 2>/dev/null | awk '/pixelWidth:/ {print $2}')"
    h="$(sips -g pixelHeight "$file" 2>/dev/null | awk '/pixelHeight:/ {print $2}')"
    if [[ -z "$w" || -z "$h" ]]; then
      echo "WARN: could not read dimensions for $(basename "$file")" >&2
      return 0
    fi
    if [[ "$w" != "$ew" || "$h" != "$eh" ]]; then
      echo "FAIL: $(basename "$file") is ${w}x${h}, expected ${ew}x${eh}" >&2
      exit 1
    fi
  }
  check_dim "$DEST/DSC02993.jpg" 1152 1440
  check_dim "$DEST/DJI_0001-20260316-184709-Pano.jpg" 1440 1440
fi
