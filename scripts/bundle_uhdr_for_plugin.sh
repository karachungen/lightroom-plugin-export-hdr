#!/usr/bin/env bash
# Build uhdr_repack (macOS 26 (Tahoe), ARM64) and install into ExportHDR.lrplugin/bin with bundled dylibs.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
UHDR_SRC="$REPO_ROOT/tools/uhdr_repack"
BUILD_DIR="$UHDR_SRC/build"
PLUGIN_BIN="$REPO_ROOT/ExportHDR.lrplugin/bin"
BUILD_EXE="$BUILD_DIR/uhdr_repack"

CMAKE_EXTRA=()
# Optional: link against a preinstalled libultrahdr (Homebrew, etc.) instead of the vendored FetchContent build.
if [[ "${UHDR_USE_SYSTEM:-}" == "1" ]] || [[ "${UHDR_USE_SYSTEM:-}" == "ON" ]]; then
	CMAKE_EXTRA+=("-DUHDR_USE_SYSTEM=ON")
fi
if [[ -n "${UHDR_ROOT:-}" ]]; then
	CMAKE_EXTRA+=("-DUHDR_ROOT=$UHDR_ROOT")
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
	echo "Requires macOS 26 (Tahoe), ARM64." >&2
	exit 1
fi

if [[ "$(uname -m)" != "arm64" ]]; then
	echo "Requires macOS 26 (Tahoe), ARM64 (this host is not arm64)." >&2
	exit 1
fi

echo "==> Configuring CMake (Release, arm64, macOS 26.0+)"
cmake -S "$UHDR_SRC" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
	"${CMAKE_EXTRA[@]+"${CMAKE_EXTRA[@]}"}"

echo "==> Building uhdr_repack"
cmake --build "$BUILD_DIR" --config Release

if [[ ! -f "$BUILD_EXE" ]]; then
	echo "Build failed: missing $BUILD_EXE" >&2
	exit 1
fi

if ! file "$BUILD_EXE" | grep -q "arm64"; then
	echo "Build output must be arm64 (macOS 26 (Tahoe), ARM64): $BUILD_EXE" >&2
	exit 1
fi

mkdir -p "$PLUGIN_BIN"
echo "==> Cleaning old bundle in $PLUGIN_BIN"
# shellcheck disable=SC2015
find "$PLUGIN_BIN" -maxdepth 1 -type f \( -name "uhdr_repack" -o -name "uhdr_repack.exe" -o -name "*.dylib" -o -name "*.dll" \) -exec rm -f {} + 2>/dev/null || true

cp "$BUILD_EXE" "$PLUGIN_BIN/uhdr_repack"
chmod +x "$PLUGIN_BIN/uhdr_repack"

# Vendored libultrahdr links as @rpath/libuhdr.*.dylib; copy dylibs before bundling so dylibbundler / otool fallback see real files.
LIBUHDR_BUILD="$BUILD_DIR/_deps/libultrahdr-build"
if [[ -d "$LIBUHDR_BUILD" ]]; then
	shopt -s nullglob
	for f in "$LIBUHDR_BUILD"/libuhdr*.dylib; do
		cp -f "$f" "$PLUGIN_BIN/"
	done
	shopt -u nullglob
	UHDR_BIN="$PLUGIN_BIN/uhdr_repack"
	while IFS= read -r dep; do
		[[ -n "$dep" ]] || continue
		case "$dep" in
		@rpath/libuhdr*.dylib)
			base="${dep##*/}"
			install_name_tool -change "$dep" "@loader_path/$base" "$UHDR_BIN" 2>/dev/null || true
			;;
		esac
	done < <(otool -L "$UHDR_BIN" | tail -n +2 | awk '{print $1}')
fi

bundle_with_dylibbundler() {
	if ! command -v dylibbundler &>/dev/null; then
		return 1
	fi
	echo "==> Bundling dependencies with dylibbundler (brew install dylibbundler)"
	(
		cd "$PLUGIN_BIN"
		# Copy linked libs into . and rewrite load paths to @loader_path/
		dylibbundler -od -b -x "./uhdr_repack" -d . -p "@loader_path/"
	)
}

# Copy Homebrew / local dylibs and rewrite ids (iterates until stable or max rounds).
bundle_fallback() {
	local dir="$PLUGIN_BIN"
	local max=6
	local round
	echo "==> Bundling dependencies (otool fallback; install dylibbundler for best results)"
	for ((round = 1; round <= max; round++)); do
		local changed=0
		local bins=()
		bins+=("$dir/uhdr_repack")
		while IFS= read -r -d '' f; do
			bins+=("$f")
		done < <(find "$dir" -maxdepth 1 -name "*.dylib" -print0 2>/dev/null || true)
		for bin in "${bins[@]}"; do
			[[ -f "$bin" ]] || continue
			chmod u+w "$bin" || true
			while IFS= read -r dep; do
				[[ -n "$dep" ]] || continue
				case "$dep" in
				@*) continue ;;
				/usr/lib/* | /System/*) continue ;;
				*/libSystem*) continue ;;
				esac
				[[ ! -f "$dep" ]] && continue
				[[ "$dep" == *.framework/* ]] && continue
				local name
				name="$(basename "$dep")"
				# Only treat as bundleable if it looks like a shared lib we can copy
				[[ "$name" == *.dylib ]] || continue
				local dest="$dir/$name"
				if [[ ! -f "$dest" ]]; then
					echo "    copy $dep"
					cp -f "$dep" "$dest"
					chmod u+w "$dest"
					changed=1
				fi
				if install_name_tool -change "$dep" "@loader_path/$name" "$bin" 2>/dev/null; then
					:
				else
					# already rewritten
					:
				fi
			done < <(otool -L "$bin" | tail -n +2 | awk '{ print $1 }')
		done
		[[ $changed -eq 0 ]] && break
	done
}

if bundle_with_dylibbundler; then
	:
else
	bundle_fallback
fi

# install_name_tool / dylibbundler invalidate the linker adhoc signature; macOS may SIGKILL the
# process at launch (Terminal shows: killed). Re-sign so Gatekeeper / AMFI accept the bundle.
adhoc_resign_bundle() {
	local dir="$1"
	echo "==> Ad-hoc codesign (required after rewriting load commands)"
	local f
	for f in "$dir"/*.dylib; do
		[[ -f "$f" ]] || continue
		codesign --force --sign - "$f"
	done
	if [[ -f "$dir/uhdr_repack" ]]; then
		codesign --force --sign - "$dir/uhdr_repack"
	fi
	if ! codesign --verify --verbose=2 "$dir/uhdr_repack" 2>/dev/null; then
		echo "Warning: codesign verify reported an issue; binary may still run." >&2
	fi
}

adhoc_resign_bundle "$PLUGIN_BIN"

echo "==> Done. Test:"
echo "    otool -L $PLUGIN_BIN/uhdr_repack | head"
echo "    $PLUGIN_BIN/uhdr_repack   # should print usage"
echo "Bundled encoder: $PLUGIN_BIN/uhdr_repack"
