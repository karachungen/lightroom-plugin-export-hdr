#!/usr/bin/env bash
# Regression test: POSIX shell invocation for plug-in paths with whitespace
# (e.g. macOS ~/Library/Application Support/Adobe/Lightroom/Modules).
# Mirrors ExportHDR.lrplugin/Command.lua shellBinary + shellQuote on non-Windows.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_DIR="$REPO_ROOT/test"
HDR="$TEST_DIR/hdr-raw.tif"
BASE="$TEST_DIR/sdr.jpg"
COMMAND_LUA="$REPO_ROOT/ExportHDR.lrplugin/Command.lua"

# Mirror CMD.shellQuote for POSIX (single-quote with '\'' escaping).
lua_shell_quote() {
	local path="$1"
	if [[ -z "$path" ]]; then
		echo "''"
		return
	fi
	local escaped="${path//\'/\'\\\'\'}"
	printf "'%s'" "$escaped"
}

find_uhdr_binary() {
	local candidates=(
		"$REPO_ROOT/ExportHDR.lrplugin/bin/uhdr_repack"
		"$REPO_ROOT/tools/uhdr_repack/build/uhdr_repack"
	)
	local c
	for c in "${candidates[@]}"; do
		if [[ -x "$c" ]]; then
			echo "$c"
			return 0
		fi
	done
	return 1
}

binary_runs() {
	local bin="$1"
	# Real encoder prints usage on unknown args; wrong-arch binaries fail to exec.
	local out
	out="$("$bin" --not-a-real-flag 2>&1 || true)"
	if echo "$out" | grep -qiE 'exec format|cannot execute|bad CPU|wrong architecture'; then
		return 1
	fi
	if echo "$out" | grep -qE 'uhdr_repack|Usage:|unknown argument'; then
		return 0
	fi
	return 1
}

install_quote_stub() {
	# Used only when the host cannot execute the real (macOS) encoder — still
	# validates shell word-splitting under Application Support paths.
	local dest="$1"
	cat >"$dest" <<'STUB'
#!/bin/sh
set -eu
inspect=0
hdr=""
base=""
out=""
while [ "$#" -gt 0 ]; do
	case "$1" in
	--inspect) inspect=1; shift; jpeg="$1"; shift ;;
	--hdr-tiff) shift; hdr="$1"; shift ;;
	--base) shift; base="$1"; shift ;;
	--out) shift; out="$1"; shift ;;
	*) shift ;;
	esac
done
if [ "$inspect" -eq 1 ]; then
	echo "is_ultra_hdr: yes"
	echo "dimensions: 1x1"
	echo "gainmap_size: 1x1"
	echo "markers: primary_xmp=yes"
	exit 0
fi
if [ -z "$base" ] || [ -z "$out" ]; then
	echo "stub: missing --base/--out" >&2
	exit 2
fi
cp "$base" "$out"
echo "Wrote $out"
echo "dimensions: 1x1"
exit 0
STUB
	chmod +x "$dest"
}

if [[ ! -f "$HDR" ]] || [[ ! -f "$BASE" ]]; then
	echo "Missing test inputs. See test/README.md — need:" >&2
	echo "  $HDR" >&2
	echo "  $BASE" >&2
	exit 3
fi

if [[ ! -f "$COMMAND_LUA" ]]; then
	echo "Missing Command.lua: $COMMAND_LUA" >&2
	exit 2
fi

STAGING_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/uhdr_Application Support_XXXXXX")"
PLUGIN_BIN="$STAGING_ROOT/ExportHDR.lrplugin/bin"
WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/uhdr_macos_quote_work_XXXXXX")"
cleanup() {
	rm -rf "$STAGING_ROOT" "$WORKDIR"
}
trap cleanup EXIT

mkdir -p "$PLUGIN_BIN"
STAGED_BIN="$PLUGIN_BIN/uhdr_repack"
USED_STUB=0

if REAL_BIN="$(find_uhdr_binary)" && binary_runs "$REAL_BIN"; then
	cp "$REAL_BIN" "$STAGED_BIN"
	# Bundle colocated dylibs when present (real macOS plugin layout).
	REAL_DIR="$(dirname "$REAL_BIN")"
	shopt -s nullglob
	for dylib in "$REAL_DIR"/*.dylib; do
		cp "$dylib" "$PLUGIN_BIN/"
	done
	shopt -u nullglob
	echo "==> Using real encoder: $REAL_BIN"
else
	install_quote_stub "$STAGED_BIN"
	USED_STUB=1
	echo "==> No runnable uhdr_repack on this host; using quoting stub at:"
	echo "    $STAGED_BIN"
fi

ENCODE_HDR="$WORKDIR/uhdr_hdr_encode.tif"
ENCODE_BASE="$WORKDIR/uhdr_sdr_base_copy.jpg"
ENCODE_OUT="$WORKDIR/uhdr_out_encode.jpg"
cp "$HDR" "$ENCODE_HDR"
cp "$BASE" "$ENCODE_BASE"

ARGS_TAIL=$(
	printf '%s ' \
		"--hdr-tiff" "$(lua_shell_quote "$ENCODE_HDR")" \
		"--base" "$(lua_shell_quote "$ENCODE_BASE")" \
		"--out" "$(lua_shell_quote "$ENCODE_OUT")" \
		"--base-quality" "92" \
		"--gainmap-quality" "85" \
		"--gainmap-scale" "1" \
		"--min-content-boost" "1" \
		"--max-content-boost" "1000" \
		"--target-display-peak" "1000"
)

echo "==> Staged plug-in bin under Application Support path:"
echo "    $PLUGIN_BIN"

# Bug repro (issue #2): unquoted absolute path is word-split by sh.
echo "==> Unquoted binary path (pre-fix shellBinary): expect failure"
UNQUOTED_ERR="$(mktemp "$WORKDIR/unquoted_err.XXXXXX")"
set +e
sh -c "$STAGED_BIN $ARGS_TAIL" >"$WORKDIR/unquoted_out.txt" 2>"$UNQUOTED_ERR"
UNQUOTED_STATUS=$?
set -e
UNQUOTED_MSG="$(cat "$UNQUOTED_ERR")"
echo "$UNQUOTED_MSG"

if [[ -f "$ENCODE_OUT" ]]; then
	echo "FAIL: unquoted path unexpectedly produced output: $ENCODE_OUT" >&2
	exit 1
fi
# macOS/bash: "No such file or directory"; Debian dash: "not found"
if ! echo "$UNQUOTED_MSG" | grep -qiE 'No such file or directory|not found'; then
	echo "FAIL: expected shell missing-file error from unquoted path." >&2
	exit 1
fi
# sh reports the truncated token ending at the space (…/Application).
if ! echo "$UNQUOTED_MSG" | grep -Fq "Application"; then
	echo "FAIL: expected shell to split on 'Application Support' space." >&2
	exit 1
fi
echo "OK: unquoted path fails as expected (issue #2)."

# Fixed behavior: CMD.shellQuote around the binary path.
QUOTED_BIN="$(lua_shell_quote "$STAGED_BIN")"
CAPTURE="$WORKDIR/uhdr_run_capture.txt"
echo "==> Quoted binary path (shellQuote): $QUOTED_BIN"
set +e
sh -c "$QUOTED_BIN $ARGS_TAIL" >"$CAPTURE" 2>&1
QUOTED_STATUS=$?
set -e
if [[ -s "$CAPTURE" ]]; then
	cat "$CAPTURE"
fi

if [[ "$QUOTED_STATUS" -ne 0 ]]; then
	echo "FAIL: quoted uhdr_repack exited $QUOTED_STATUS (expected 0)." >&2
	exit 1
fi
if [[ ! -f "$ENCODE_OUT" ]]; then
	echo "FAIL: staged output JPEG missing: $ENCODE_OUT" >&2
	exit 1
fi
if ! grep -q "Wrote " "$CAPTURE"; then
	echo "FAIL: encoder output missing expected 'Wrote' line." >&2
	exit 1
fi
echo "OK: shellQuote'd binary path works under Application Support."

INSPECT_CAPTURE="$WORKDIR/uhdr_inspect_capture.txt"
set +e
sh -c "$QUOTED_BIN --inspect $(lua_shell_quote "$ENCODE_OUT")" >"$INSPECT_CAPTURE" 2>&1
INSPECT_STATUS=$?
set -e
if [[ -s "$INSPECT_CAPTURE" ]]; then
	cat "$INSPECT_CAPTURE"
fi
if [[ "$INSPECT_STATUS" -ne 0 ]]; then
	echo "FAIL: quoted --inspect exited $INSPECT_STATUS." >&2
	exit 1
fi
if ! grep -q "is_ultra_hdr: yes" "$INSPECT_CAPTURE"; then
	echo "FAIL: quoted --inspect did not report is_ultra_hdr: yes." >&2
	exit 1
fi
echo "OK: quoted --inspect reports is_ultra_hdr: yes."

# Regression guard: plugin must shellQuote the macOS binary token (issue #2 fix).
echo "==> Assert Command.lua shellBinary quotes the macOS binary path"
if ! awk '
	BEGIN { in_fn = 0 }
	/^function CMD\.shellBinary\(binary\)/ { in_fn = 1; next }
	in_fn && /^end$/ {
		if (!found) { exit 2 }
		exit 0
	}
	in_fn && /return CMD\.shellQuote\(binary or CMD\.bundledBinaryPath\(\)\)/ { found = 1 }
' "$COMMAND_LUA"; then
	echo "FAIL: CMD.shellBinary does not return CMD.shellQuote(binary or CMD.bundledBinaryPath())." >&2
	echo "      macOS installs under Application Support still break (issue #2)." >&2
	exit 1
fi
echo "OK: Command.lua shellBinary uses shellQuote on macOS."

if [[ "$USED_STUB" -eq 1 ]]; then
	echo "NOTE: quoting stub was used; run on macOS after build_plugin.sh for a real encode."
fi

exit 0
