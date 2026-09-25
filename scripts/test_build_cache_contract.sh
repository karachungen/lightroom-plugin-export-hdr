#!/usr/bin/env bash
# Contract between Qt stamp files, install scripts, and the release workflow.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail() { echo "contract: $*" >&2; exit 1; }

stamp_has() {
	local file="$1" key="$2" value="$3"
	grep -qx "${key}=${value}" "$file" || fail "$file missing ${key}=${value}"
}

mac="$ROOT/scripts/qt/macos-arm64.stamp"
win="$ROOT/scripts/qt/windows-x64.stamp"
[[ -f "$mac" ]] || fail "missing $mac"
[[ -f "$win" ]] || fail "missing $win"
stamp_has "$mac" version 6.11.2
stamp_has "$mac" linkage static
stamp_has "$mac" arch arm64
stamp_has "$mac" deployment_target 13.0
stamp_has "$mac" submodules qtbase,qtshadertools
stamp_has "$win" version 6.11.2
stamp_has "$win" linkage shared
stamp_has "$win" arch x64
stamp_has "$win" aqt_arch win64_msvc2022_64
stamp_has "$win" modules qtshadertools
grep -qx 'scripts/qt/macos-arm64.stamp text eol=lf' "$ROOT/.gitattributes" || fail "macos stamp is not eol=lf"
grep -qx 'scripts/qt/windows-x64.stamp text eol=lf' "$ROOT/.gitattributes" || fail "windows stamp is not eol=lf"
setup="$ROOT/scripts/setup_qt_static.sh"
grep -q 'scripts/qt/macos-arm64.stamp' "$setup" || fail "setup_qt_static.sh does not read the macOS stamp"
grep -q 'QT_NO_XCODE_MIN_VERSION_CHECK=ON' "$setup" || fail "Xcode check skip is not unconditional"
if grep -q 'GITHUB_ACTIONS' "$setup"; then
	fail "setup_qt_static.sh still branches on GITHUB_ACTIONS"
fi
if grep -q 'QT_VERSION:-6.11.2' "$setup"; then
	fail "setup_qt_static.sh still hardcodes the Qt version"
fi
echo "ok build cache contract"
