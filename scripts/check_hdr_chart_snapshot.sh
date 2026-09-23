#!/usr/bin/env bash
# Encode the HDR chart and require the Ultra HDR JPEG to match the committed snapshot.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CHART_DIR="$REPO_ROOT/test/hdr-chart"
SNAPSHOT="$CHART_DIR/chart-uhdr.snapshot.jpg"
ENCODED="$CHART_DIR/chart-uhdr.jpg"

is_windows_shell() {
	case "$(uname -s)" in
	MINGW* | MSYS* | CYGWIN* | Windows_NT) return 0 ;;
	esac
	return 1
}

find_uhdr_bin() {
	local plugin_bin="$REPO_ROOT/ExportHDR.lrplugin/bin"
	local build_dir="$REPO_ROOT/tools/uhdr_repack/build"

	if is_windows_shell; then
		if [[ -f "$plugin_bin/uhdr_repack.exe" ]]; then
			echo "$plugin_bin/uhdr_repack.exe"
			return 0
		fi
		if [[ -f "$build_dir/uhdr_repack.exe" ]]; then
			echo "$build_dir/uhdr_repack.exe"
			return 0
		fi
		if [[ -f "$build_dir/Release/uhdr_repack.exe" ]]; then
			echo "$build_dir/Release/uhdr_repack.exe"
			return 0
		fi
		return 1
	fi

	if [[ -x "$plugin_bin/uhdr_repack" ]]; then
		echo "$plugin_bin/uhdr_repack"
		return 0
	fi
	if [[ -x "$build_dir/uhdr_repack" ]]; then
		echo "$build_dir/uhdr_repack"
		return 0
	fi
	return 1
}

if [[ ! -f "$SNAPSHOT" ]]; then
	echo "missing snapshot: $SNAPSHOT" >&2
	exit 1
fi

BIN=""
if ! BIN="$(find_uhdr_bin)"; then
	echo "uhdr_repack not found. Build with ./scripts/build_plugin.sh" >&2
	exit 2
fi

if [[ "$BIN" == *.exe ]]; then
	export PATH="$(dirname "$BIN"):$PATH"
fi

log="$(mktemp)"
set +e
"$BIN" --check-hdr-chart "$CHART_DIR" >"$log" 2>&1
status=$?
set -e
cat "$log"

if [[ ! -f "$ENCODED" ]]; then
	echo "encode did not write $ENCODED (exit $status)" >&2
	rm -f "$log"
	exit 1
fi
if ! grep -q "R2020 +4 gain RGB .* PASS" "$log"; then
	echo "R2020 +4 chromatic gain gate did not pass" >&2
	rm -f "$log"
	exit 1
fi
rm -f "$log"

if ! cmp -s "$SNAPSHOT" "$ENCODED"; then
	echo "HDR chart snapshot mismatch" >&2
	echo "snapshot: $(wc -c <"$SNAPSHOT" | tr -d ' ') bytes" >&2
	echo "encoded:  $(wc -c <"$ENCODED" | tr -d ' ') bytes" >&2
	exit 1
fi

echo "OK: chart-uhdr.jpg matches chart-uhdr.snapshot.jpg"
