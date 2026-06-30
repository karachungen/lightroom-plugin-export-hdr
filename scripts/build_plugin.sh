#!/usr/bin/env bash
# Unified build orchestrator for ExportHDR.lrplugin (macOS ARM64 + Windows x64).
# Uses CMake presets in tools/uhdr_repack/CMakePresets.json — same path for local and CI.
#
# Usage:
#   ./scripts/build_plugin.sh [install-deps|build|bundle|test|package|all] [--preset NAME] [--clean]
set -euo pipefail

# Pin CMake 3.31.x on Windows (CMake 4.x breaks vendored libjpeg-turbo).
REQUIRED_CMAKE_VERSION_PREFIX="3.31."

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
UHDR_SRC="$REPO_ROOT/tools/uhdr_repack"
BUILD_DIR="$UHDR_SRC/build"
PLUGIN_BIN="$REPO_ROOT/ExportHDR.lrplugin/bin"

CLEAN=0
PRESET=""
COMMAND=""

usage() {
	cat <<'EOF'
Usage: build_plugin.sh [install-deps|build|bundle|test|package|all] [--preset NAME] [--clean]

  install-deps  Install platform build dependencies (macOS: brew; Windows local: setup_windows_build.ps1)
  build         cmake --preset + cmake --build
  bundle        Copy encoder + runtime libs into ExportHDR.lrplugin/bin
  test          Run scripts/run_uhdr_test.sh
  package       Create platform zip via scripts/package_plugin.sh
  all           build → bundle → test → package

Options:
  --preset NAME   Override auto-detected preset (macos-arm64-release | windows-x64-release)
  --clean         Remove tools/uhdr_repack/build before configure
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	install-deps | build | bundle | test | package | all)
		COMMAND="$1"
		shift
		;;
	--preset)
		PRESET="$2"
		shift 2
		;;
	--clean)
		CLEAN=1
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "Unknown argument: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

if [[ -z "$COMMAND" ]]; then
	COMMAND="all"
fi

detect_preset() {
	case "$(uname -s)" in
	Darwin)
		if [[ "$(uname -m)" != "arm64" ]]; then
			echo "Requires macOS ARM64 (this host is $(uname -m))." >&2
			exit 1
		fi
		echo "macos-arm64-release"
		;;
	MINGW* | MSYS* | CYGWIN* | Windows_NT)
		echo "windows-x64-release"
		;;
	*)
		echo "Unsupported host OS: $(uname -s)" >&2
		exit 1
		;;
	esac
}

detect_platform_id() {
	case "$(uname -s)" in
	Darwin) echo "macos-arm64" ;;
	MINGW* | MSYS* | CYGWIN* | Windows_NT) echo "windows-x64" ;;
	*) echo "unknown" ;;
	esac
}

is_windows_host() {
	case "$(uname -s)" in
	MINGW* | MSYS* | CYGWIN* | Windows_NT) return 0 ;;
	*) return 1 ;;
	esac
}

if [[ -z "$PRESET" ]]; then
	PRESET="$(detect_preset)"
fi

cmake_extra=()
if [[ "${UHDR_USE_SYSTEM:-}" == "1" || "${UHDR_USE_SYSTEM:-}" == "ON" ]]; then
	cmake_extra+=("-DUHDR_USE_SYSTEM=ON")
fi
if [[ -n "${UHDR_ROOT:-}" ]]; then
	cmake_extra+=("-DUHDR_ROOT=$UHDR_ROOT")
fi

assert_cmake_version() {
	if ! command -v cmake &>/dev/null; then
		echo "cmake not found on PATH." >&2
		exit 1
	fi
	if is_windows_host; then
		local ver_line
		ver_line="$(cmake --version 2>/dev/null | head -n 1)"
		if [[ "$ver_line" != *"version ${REQUIRED_CMAKE_VERSION_PREFIX}"* ]]; then
			echo "Windows requires CMake ${REQUIRED_CMAKE_VERSION_PREFIX}x ($ver_line). Run .\\scripts\\setup_windows_build.ps1" >&2
			exit 1
		fi
	fi
}

cmd_install_deps() {
	case "$(uname -s)" in
	Darwin)
		echo "==> Installing macOS build dependencies (brew)"
		if ! command -v brew &>/dev/null; then
			echo "Homebrew is required. See https://brew.sh" >&2
			exit 1
		fi
		brew install cmake jpeg-turbo dylibbundler
		;;
	MINGW* | MSYS* | CYGWIN* | Windows_NT)
		if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
			echo "==> Windows CI: dependencies provided by workflow actions (skipping install-deps)"
			return 0
		fi
		echo "==> Installing Windows build dependencies (setup_windows_build.ps1)"
		powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$SCRIPT_DIR/setup_windows_build.ps1"
		;;
	*)
		echo "install-deps not supported on $(uname -s)" >&2
		exit 1
		;;
	esac
}

cmd_build() {
	assert_cmake_version
	if [[ "$CLEAN" -eq 1 ]] && [[ -d "$BUILD_DIR" ]]; then
		echo "==> Cleaning $BUILD_DIR"
		rm -rf "$BUILD_DIR"
	fi

	echo "==> Configuring preset: $PRESET"
	if [[ ${#cmake_extra[@]} -gt 0 ]]; then
		cmake --preset "$PRESET" -S "$UHDR_SRC" "${cmake_extra[@]}"
	else
		cmake --preset "$PRESET" -S "$UHDR_SRC"
	fi

	echo "==> Building preset: $PRESET"
	cmake --build "$BUILD_DIR"
}

find_build_exe() {
	local candidates=()
	case "$PRESET" in
	macos-arm64-release)
		candidates+=("$BUILD_DIR/uhdr_repack")
		;;
	windows-x64-release)
		candidates+=("$BUILD_DIR/uhdr_repack.exe" "$BUILD_DIR/Release/uhdr_repack.exe")
		;;
	esac
	local c
	for c in "${candidates[@]}"; do
		if [[ -f "$c" ]]; then
			echo "$c"
			return 0
		fi
	done
	return 1
}

clean_plugin_bin() {
	mkdir -p "$PLUGIN_BIN"
	if is_windows_host; then
		# MSYS: rm uhdr_repack can delete uhdr_repack.exe — remove by explicit extension only.
		rm -f "$PLUGIN_BIN/uhdr_repack.exe" "$PLUGIN_BIN"/*.dll "$PLUGIN_BIN"/*.dylib 2>/dev/null || true
	else
		find "$PLUGIN_BIN" -maxdepth 1 -type f \( \
			-name "uhdr_repack" -o -name "uhdr_repack.exe" -o -name "*.dylib" -o -name "*.dll" \
			\) -exec rm -f {} + 2>/dev/null || true
	fi
}

bundle_macos() {
	local build_exe
	build_exe="$(find_build_exe)" || {
		echo "Build failed: missing uhdr_repack under $BUILD_DIR" >&2
		exit 1
	}

	if ! file "$build_exe" | grep -q "arm64"; then
		echo "Build output must be arm64: $build_exe" >&2
		exit 1
	fi

	echo "==> Cleaning old bundle in $PLUGIN_BIN"
	clean_plugin_bin
	cp "$build_exe" "$PLUGIN_BIN/uhdr_repack"
	chmod +x "$PLUGIN_BIN/uhdr_repack"

	local libuhdr_build="$BUILD_DIR/_deps/libultrahdr-build"
	if [[ -d "$libuhdr_build" ]]; then
		shopt -s nullglob
		local f
		for f in "$libuhdr_build"/libuhdr*.dylib; do
			cp -f "$f" "$PLUGIN_BIN/"
		done
		shopt -u nullglob
		local uhdr_bin="$PLUGIN_BIN/uhdr_repack"
		local dep
		while IFS= read -r dep; do
			[[ -n "$dep" ]] || continue
			case "$dep" in
			@rpath/libuhdr*.dylib)
				local base="${dep##*/}"
				install_name_tool -change "$dep" "@loader_path/$base" "$uhdr_bin" 2>/dev/null || true
				;;
			esac
		done < <(otool -L "$uhdr_bin" | tail -n +2 | awk '{print $1}')
	fi

	bundle_with_dylibbundler() {
		if ! command -v dylibbundler &>/dev/null; then
			return 1
		fi
		echo "==> Bundling dependencies with dylibbundler"
		(
			cd "$PLUGIN_BIN"
			dylibbundler -od -b -x "./uhdr_repack" -d . -p "@loader_path/"
		)
	}

	bundle_fallback() {
		local dir="$PLUGIN_BIN"
		local max=6 round
		echo "==> Bundling dependencies (otool fallback; install dylibbundler for best results)"
		for ((round = 1; round <= max; round++)); do
			local changed=0
			local bins=("$dir/uhdr_repack")
			while IFS= read -r -d '' f; do
				bins+=("$f")
			done < <(find "$dir" -maxdepth 1 -name "*.dylib" -print0 2>/dev/null || true)
			local bin dep name dest
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
					name="$(basename "$dep")"
					[[ "$name" == *.dylib ]] || continue
					dest="$dir/$name"
					if [[ ! -f "$dest" ]]; then
						echo "    copy $dep"
						cp -f "$dep" "$dest"
						chmod u+w "$dest"
						changed=1
					fi
					install_name_tool -change "$dep" "@loader_path/$name" "$bin" 2>/dev/null || true
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

	echo "==> Ad-hoc codesign (required after rewriting load commands)"
	local f
	for f in "$PLUGIN_BIN"/*.dylib; do
		[[ -f "$f" ]] || continue
		codesign --force --sign - "$f"
	done
	if [[ -f "$PLUGIN_BIN/uhdr_repack" ]]; then
		codesign --force --sign - "$PLUGIN_BIN/uhdr_repack"
	fi

	echo "==> Bundled encoder: $PLUGIN_BIN/uhdr_repack"
}

bundle_windows() {
	local build_exe
	build_exe="$(find_build_exe)" || {
		echo "Build failed: missing uhdr_repack.exe under $BUILD_DIR" >&2
		exit 1
	}

	echo "==> Cleaning old Windows bundle in $PLUGIN_BIN"
	clean_plugin_bin
	cp "$build_exe" "$PLUGIN_BIN/uhdr_repack.exe"

	local roots=(
		"$BUILD_DIR"
		"$BUILD_DIR/Release"
		"$BUILD_DIR/_deps/libultrahdr-build"
		"$BUILD_DIR/_deps/libultrahdr-build/Release"
	)
	local root dll name
	for root in "${roots[@]}"; do
		[[ -d "$root" ]] || continue
		shopt -s nullglob
		for dll in "$root"/*.dll; do
			name="$(basename "$dll")"
			if [[ ! -f "$PLUGIN_BIN/$name" ]]; then
				cp -f "$dll" "$PLUGIN_BIN/$name"
				echo "    bundled $name"
			fi
		done
		shopt -u nullglob
	done

	local plugin_exe="$PLUGIN_BIN/uhdr_repack.exe"
	echo "==> Smoke: uhdr_repack.exe (usage if no args)"
	"$plugin_exe" 2>&1 >/dev/null || true
	local ec=$?
	if [[ "$ec" -ne 1 ]]; then
		echo "Warning: expected usage exit code 1 when run without args; got $ec" >&2
	fi

	echo "==> Bundled encoder: $plugin_exe"
}

cmd_bundle() {
	case "$PRESET" in
	macos-arm64-release) bundle_macos ;;
	windows-x64-release) bundle_windows ;;
	*)
		echo "Unknown preset for bundle: $PRESET" >&2
		exit 1
		;;
	esac
}

cmd_test() {
	"$SCRIPT_DIR/run_uhdr_test.sh"
}

cmd_package() {
	"$SCRIPT_DIR/package_plugin.sh" "$(detect_platform_id)"
}

case "$COMMAND" in
install-deps) cmd_install_deps ;;
build) cmd_build ;;
bundle) cmd_bundle ;;
test) cmd_test ;;
package) cmd_package ;;
all)
	cmd_build
	cmd_bundle
	cmd_test
	cmd_package
	;;
*)
	echo "Unknown command: $COMMAND" >&2
	usage >&2
	exit 2
	;;
esac
