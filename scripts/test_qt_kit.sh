#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/qt_kit.sh
source "$ROOT/scripts/qt_kit.sh"

TMP=""
cleanup() { if [[ -n "$TMP" ]]; then rm -rf "$TMP"; fi; }
trap cleanup EXIT

new_case() {
	cleanup
	TMP="$(mktemp -d)"
	export UHDR_BUILD_CACHE_DIR="$TMP/cache"
	export QT_STATIC_ROOT="$TMP/prefix"
	export QT_ROOT_DIR="$TMP/6.11.2/msvc2022_64"
	unset UHDR_QT_CACHE_RESTORED || true
	rm -f "$TMP/installed"
	mkdir -p "$UHDR_BUILD_CACHE_DIR"
}

fake_qmake_into() {
	local prefix="$1"
	mkdir -p "$prefix/bin" "$prefix/lib/cmake/Qt6"
	printf 'ok\n' > "$prefix/lib/cmake/Qt6/Qt6Config.cmake"
	cat > "$prefix/bin/qmake6" <<'EOF'
#!/bin/sh
echo 6.11.2
EOF
	chmod +x "$prefix/bin/qmake6"
}

qt_install_kit() {
	echo called > "$TMP/installed"
	fake_qmake_into "$2"
}

assert_no_install() {
	[[ ! -f "$TMP/installed" ]] || { echo "install was called" >&2; exit 1; }
}

# Ready prefix.
new_case
fake_qmake_into "$QT_STATIC_ROOT"
cp "$ROOT/scripts/qt/macos-arm64.stamp" "$QT_STATIC_ROOT/.uhdr-qt-stamp"
out="$(ensure_qt_kit macos-arm64)"
[[ "$out" == "==> Qt kit ready at $QT_STATIC_ROOT" ]] || { echo "$out" >&2; exit 1; }
assert_no_install

# Adopt same version, write marker and archive.
new_case
fake_qmake_into "$QT_STATIC_ROOT"
out="$(ensure_qt_kit macos-arm64)"
[[ "$out" == "==> Qt kit ready at $QT_STATIC_ROOT" ]] || { echo "$out" >&2; exit 1; }
cmp -s "$QT_STATIC_ROOT/.uhdr-qt-stamp" "$ROOT/scripts/qt/macos-arm64.stamp"
[[ -f "$UHDR_BUILD_CACHE_DIR/qt-macos-arm64.tar.zst" ]]
assert_no_install

# Extract a good archive.
new_case
fake_qmake_into "$TMP/src"
cp "$ROOT/scripts/qt/macos-arm64.stamp" "$TMP/src/.uhdr-qt-stamp"
qt_write_archive "$TMP/src" "$UHDR_BUILD_CACHE_DIR/qt-macos-arm64.tar.zst"
out="$(ensure_qt_kit macos-arm64)"
[[ "$out" == "==> Extracting Qt kit from $UHDR_BUILD_CACHE_DIR/qt-macos-arm64.tar.zst" ]] || { echo "$out" >&2; exit 1; }
[[ -f "$QT_STATIC_ROOT/lib/cmake/Qt6/Qt6Config.cmake" ]]
assert_no_install

# Restored bad archive fails and names the stamp.
new_case
mkdir -p "$TMP/empty"
qt_write_archive "$TMP/empty" "$UHDR_BUILD_CACHE_DIR/qt-macos-arm64.tar.zst"
export UHDR_QT_CACHE_RESTORED=true
set +e
out="$(ensure_qt_kit macos-arm64 2>&1)"
ec=$?
set -e
[[ "$ec" -eq 1 ]]
[[ "$out" == *"scripts/qt/macos-arm64.stamp"* ]]
assert_no_install

# Bad local archive is deleted and install runs.
new_case
mkdir -p "$TMP/empty"
qt_write_archive "$TMP/empty" "$UHDR_BUILD_CACHE_DIR/qt-macos-arm64.tar.zst"
out="$(ensure_qt_kit macos-arm64)"
[[ "$out" == "==> Qt kit ready at $QT_STATIC_ROOT" ]] || { echo "$out" >&2; exit 1; }
[[ -f "$TMP/installed" ]]
[[ -f "$UHDR_BUILD_CACHE_DIR/qt-macos-arm64.tar.zst" ]]
cmp -s "$QT_STATIC_ROOT/.uhdr-qt-stamp" "$ROOT/scripts/qt/macos-arm64.stamp"

# No prefix and no archive installs.
new_case
out="$(ensure_qt_kit macos-arm64)"
[[ "$out" == "==> Qt kit ready at $QT_STATIC_ROOT" ]]
[[ -f "$TMP/installed" ]]

# Missing zstd when an archive must be written.
new_case
fake_qmake_into "$QT_STATIC_ROOT"
bindir="$TMP/bin"
mkdir -p "$bindir"
ln -s "$(command -v tar)" "$bindir/tar"
set +e
out="$(PATH="$bindir" ensure_qt_kit macos-arm64 2>&1)"
ec=$?
set -e
[[ "$ec" -eq 1 ]]
[[ "$out" == *"install-deps"* ]]

# Windows incomplete tree deletes the version directory, then installs.
new_case
mkdir -p "$QT_ROOT_DIR/partial"
out="$(ensure_qt_kit windows-x64)"
[[ "$out" == "==> Qt kit ready at $QT_ROOT_DIR" ]] || { echo "$out" >&2; exit 1; }
[[ ! -d "$TMP/6.11.2/partial" ]]
[[ -f "$TMP/installed" ]]

echo "ok qt kit"
