#!/usr/bin/env bash
# Package ExportHDR.lrplugin into a platform-specific zip archive.
# Usage: ./scripts/package_plugin.sh [macos-arm64|windows-x64]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLUGIN_DIR="$REPO_ROOT/ExportHDR.lrplugin"
PLUGIN_BIN="$PLUGIN_DIR/bin"

usage() {
	cat <<'EOF'
Usage: package_plugin.sh [macos-arm64|windows-x64]

Creates ExportHDR.lrplugin-macos-arm64.zip or ExportHDR.lrplugin-windows-x64.zip
in the repo root. Strips binaries for the other platform and excludes bin/.gitignore
and bin/README.txt from the archive.
EOF
}

detect_platform() {
	case "$(uname -s)" in
	Darwin)
		if [[ "$(uname -m)" != "arm64" ]]; then
			echo "macOS packaging requires arm64 host." >&2
			exit 1
		fi
		echo "macos-arm64"
		;;
	MINGW* | MSYS* | CYGWIN* | Windows_NT)
		echo "windows-x64"
		;;
	*)
		echo "Unsupported host for packaging: $(uname -s)" >&2
		exit 1
		;;
	esac
}

platform="${1:-}"
if [[ -z "$platform" ]]; then
	platform="$(detect_platform)"
fi

case "$platform" in
macos-arm64)
	artifact="ExportHDR.lrplugin-macos-arm64.zip"
	rm -f "$PLUGIN_BIN/uhdr_repack.exe"
	rm -f "$PLUGIN_BIN"/*.dll
	if [[ ! -x "$PLUGIN_BIN/uhdr_repack" ]]; then
		echo "Missing macOS binary: $PLUGIN_BIN/uhdr_repack" >&2
		exit 1
	fi
	;;
windows-x64)
	artifact="ExportHDR.lrplugin-windows-x64.zip"
	# Do not rm "$PLUGIN_BIN/uhdr_repack" on MSYS — it can delete uhdr_repack.exe (same base name).
	rm -f "$PLUGIN_BIN"/*.dylib
	if [[ -f "$PLUGIN_BIN/uhdr_repack" ]] && [[ ! -f "$PLUGIN_BIN/uhdr_repack.exe" ]]; then
		rm -f "$PLUGIN_BIN/uhdr_repack"
	fi
	if [[ ! -f "$PLUGIN_BIN/uhdr_repack.exe" ]]; then
		echo "Missing Windows binary: $PLUGIN_BIN/uhdr_repack.exe" >&2
		exit 1
	fi
	;;
*)
	echo "Unknown platform: $platform" >&2
	usage >&2
	exit 2
	;;
esac

out_zip="$REPO_ROOT/$artifact"
rm -f "$out_zip"

case "$platform" in
macos-arm64)
	(
		cd "$REPO_ROOT"
		zip -r "$artifact" ExportHDR.lrplugin \
			-x "ExportHDR.lrplugin/bin/.gitignore" \
			-x "ExportHDR.lrplugin/bin/README.txt"
	)
	;;
windows-x64)
	if command -v zip &>/dev/null; then
		(
			cd "$REPO_ROOT"
			zip -r "$artifact" ExportHDR.lrplugin \
				-x "ExportHDR.lrplugin/bin/.gitignore" \
				-x "ExportHDR.lrplugin/bin/README.txt"
		)
	else
		# Git Bash on Windows may lack zip; use PowerShell Compress-Archive with temp staging.
		staging="$(mktemp -d)"
		trap 'rm -rf "$staging"' EXIT
		cp -R "$PLUGIN_DIR" "$staging/"
		rm -f "$staging/ExportHDR.lrplugin/bin/.gitignore" \
			"$staging/ExportHDR.lrplugin/bin/README.txt" 2>/dev/null || true
		staging_plugin="$staging/ExportHDR.lrplugin"
		if command -v cygpath &>/dev/null; then
			staging_plugin="$(cygpath -w "$staging_plugin")"
			out_zip_win="$(cygpath -w "$out_zip")"
		else
			staging_plugin="$(cd "$staging" && pwd -W 2>/dev/null || pwd)/ExportHDR.lrplugin"
			out_zip_win="$(cd "$REPO_ROOT" && pwd -W 2>/dev/null || pwd)/$artifact"
		fi
		powershell.exe -NoProfile -Command \
			"Compress-Archive -LiteralPath '$staging_plugin' -DestinationPath '$out_zip_win' -Force"
	fi
	;;
esac

echo "Created $out_zip"
