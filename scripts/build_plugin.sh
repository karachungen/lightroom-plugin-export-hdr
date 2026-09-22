#!/usr/bin/env bash
# Unified build orchestrator for ExportHDR.lrplugin (macOS ARM64 + Windows x64).
# Uses CMake presets in tools/uhdr_repack/CMakePresets.json — same path for local and CI.
#
# Usage:
#   ./scripts/build_plugin.sh [install-deps|install|build|bundle|test|package|all] [--preset NAME] [--clean]
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
Usage: build_plugin.sh [install-deps|install|build|bundle|test|package|all] [--preset NAME] [--clean]

  install-deps  Install platform build dependencies (macOS: brew; Windows local: setup_windows_build.ps1)
  install       build → bundle → test (default; updates ExportHDR.lrplugin in place, no zip)
  build         cmake --preset + cmake --build
  bundle        Copy encoder + runtime libs into ExportHDR.lrplugin/bin
  test          Run scripts/run_uhdr_test.sh
  package       Create platform zip via scripts/package_plugin.sh
  all           build → bundle → test → package (CI / release)

Options:
  --preset NAME   Override auto-detected preset (macos-arm64-release | windows-x64-release)
  --clean         Remove tools/uhdr_repack/build before configure
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	install-deps | install | build | bundle | test | package | all)
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
	COMMAND="install"
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
if [[ "${UHDR_ENABLE_GUI:-}" == "0" || "${UHDR_ENABLE_GUI:-}" == "OFF" ]]; then
	cmake_extra+=("-DUHDR_ENABLE_GUI=OFF")
fi
QT_RESOLVED=0

qt_config_present() {
	[[ -f "$1/lib/cmake/Qt6/Qt6Config.cmake" ]]
}

qt_version_at_prefix() {
	local prefix="$1"
	local qmake=""
	if [[ -x "$prefix/bin/qmake6" ]]; then
		qmake="$prefix/bin/qmake6"
	elif command -v qmake6 &>/dev/null; then
		local reported
		reported="$(qtpaths6 --install-prefix 2>/dev/null || true)"
		if [[ "$reported" == "$prefix" ]]; then
			qmake="$(command -v qmake6)"
		fi
	fi
	if [[ -z "$qmake" ]]; then
		return 1
	fi
	"$qmake" -query QT_VERSION 2>/dev/null
}

qt_version_ge_611() {
	local ver="$1" major minor
	IFS='.' read -r major minor _ <<<"$ver"
	[[ "$major" -gt 6 ]] && return 0
	[[ "$major" -eq 6 && "$minor" -ge 11 ]] && return 0
	return 1
}

find_qt_static_root() {
	local candidates=()
	if [[ -n "${QT_STATIC_ROOT:-}" ]]; then
		candidates+=("$QT_STATIC_ROOT")
	fi
	candidates+=("$HOME/Qt/${QT_VERSION:-6.11.2}-static")

	local candidate
	for candidate in "${candidates[@]}"; do
		if qt_config_present "$candidate"; then
			echo "$candidate"
			return 0
		fi
	done
	return 1
}

find_qt_shared_prefix() {
	local candidates=() candidate seen="" version

	if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
		local path_entry
		IFS=':' read -ra path_entries <<<"${CMAKE_PREFIX_PATH}"
		for path_entry in "${path_entries[@]}"; do
			[[ -n "$path_entry" ]] && candidates+=("$path_entry")
		done
	fi

	if [[ -n "${QT_ROOT_DIR:-}" ]]; then
		candidates+=("$QT_ROOT_DIR")
	fi

	if command -v brew &>/dev/null; then
		local brew_qt
		brew_qt="$(brew --prefix qt 2>/dev/null || true)"
		[[ -n "$brew_qt" ]] && candidates+=("$brew_qt")
	fi

	if command -v qtpaths6 &>/dev/null; then
		local qt_prefix
		qt_prefix="$(qtpaths6 --install-prefix 2>/dev/null || true)"
		[[ -n "$qt_prefix" ]] && candidates+=("$qt_prefix")
	fi

	local qt_install_dir
	for qt_install_dir in "$HOME/Qt"/*/macos; do
		[[ -d "$qt_install_dir" ]] && candidates+=("$qt_install_dir")
	done

	for candidate in "${candidates[@]}"; do
		[[ "$seen" == *"|$candidate|"* ]] && continue
		seen="${seen}|$candidate|"
		if ! qt_config_present "$candidate"; then
			continue
		fi
		version="$(qt_version_at_prefix "$candidate" || true)"
		if [[ -n "$version" ]] && ! qt_version_ge_611 "$version"; then
			continue
		fi
		echo "$candidate"
		return 0
	done
	return 1
}

resolve_qt_for_build() {
	if [[ "$QT_RESOLVED" -eq 1 ]]; then
		return 0
	fi

	if [[ "${UHDR_ENABLE_GUI:-ON}" == "0" || "${UHDR_ENABLE_GUI:-ON}" == "OFF" ]]; then
		QT_RESOLVED=1
		return 0
	fi

	local static_root shared_prefix

	if [[ "${UHDR_STATIC_QT:-ON}" == "0" || "${UHDR_STATIC_QT:-ON}" == "OFF" ]]; then
		shared_prefix="$(find_qt_shared_prefix || true)"
		if [[ -z "$shared_prefix" ]]; then
			echo "UHDR_STATIC_QT=OFF but no Qt 6.11+ installation was found." >&2
			echo "Install Qt locally (e.g. brew install qt) or set CMAKE_PREFIX_PATH." >&2
			exit 1
		fi
		cmake_extra+=("-DUHDR_STATIC_QT=OFF" "-DCMAKE_PREFIX_PATH=$shared_prefix")
		echo "==> Using shared Qt at $shared_prefix (development build)"
		QT_RESOLVED=1
		return 0
	fi

	static_root="$(find_qt_static_root || true)"
	if [[ -n "$static_root" ]]; then
		cmake_extra+=("-DQT_STATIC_ROOT=$static_root" "-DUHDR_STATIC_QT=ON")
		echo "==> Using static Qt at $static_root"
		QT_RESOLVED=1
		return 0
	fi

	if [[ "${UHDR_REQUIRE_STATIC_QT:-}" == "1" || "${UHDR_REQUIRE_STATIC_QT:-}" == "ON" ]]; then
		echo "UHDR_REQUIRE_STATIC_QT is set but no static Qt kit was found." >&2
		echo "Run ./scripts/setup_qt_static.sh or set QT_STATIC_ROOT." >&2
		exit 1
	fi

	shared_prefix="$(find_qt_shared_prefix || true)"
	if [[ -n "$shared_prefix" ]]; then
		cmake_extra+=("-DUHDR_STATIC_QT=OFF" "-DCMAKE_PREFIX_PATH=$shared_prefix")
		echo "==> Using shared Qt at $shared_prefix (local development build)"
		echo "    For a release single-file binary, run ./scripts/setup_qt_static.sh and export QT_STATIC_ROOT." >&2
		QT_RESOLVED=1
		return 0
	fi

	echo "No Qt 6.11+ installation found." >&2
	echo "Install Qt locally (e.g. brew install qt), or run ./scripts/setup_qt_static.sh for a static release kit." >&2
	exit 1
}

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
		brew install cmake ninja qt
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
	resolve_qt_for_build
	if [[ "$CLEAN" -eq 1 ]] && [[ -d "$BUILD_DIR" ]]; then
		echo "==> Cleaning $BUILD_DIR"
		rm -rf "$BUILD_DIR"
	fi

	# Vendored libjpeg-turbo's cmake_minimum_required() predates CMake 4's removal of
	# compatibility with CMake < 3.5; this tells CMake to treat it as 3.5 instead of erroring.
	export CMAKE_POLICY_VERSION_MINIMUM=3.5

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
	local plugin_root
	plugin_root="$(dirname "$PLUGIN_BIN")"
	mkdir -p "$PLUGIN_BIN"
	if is_windows_host; then
		# MSYS: rm uhdr_repack can delete uhdr_repack.exe — remove by explicit extension only.
		rm -f "$PLUGIN_BIN/uhdr_repack.exe" "$PLUGIN_BIN"/*.dll "$PLUGIN_BIN"/*.dylib 2>/dev/null || true
	else
		find "$PLUGIN_BIN" -maxdepth 1 -type f \( \
			-name "uhdr_repack" -o -name "uhdr_repack.exe" -o -name "*.dylib" -o -name "*.dll" \
			\) -exec rm -f {} + 2>/dev/null || true
		rm -rf "$PLUGIN_BIN"/{Resources,lib,share,tls,translations} 2>/dev/null || true
	fi
	rm -rf "$plugin_root"/{Frameworks,PlugIns} 2>/dev/null || true
}

uhdr_links_shared_qt_macos() {
	local exe="$1"
	otool -L "$exe" 2>/dev/null | grep -qE '[[:space:]]+(@rpath/|.*/)(Qt[A-Za-z0-9_-]*\.framework|Qt[A-Za-z0-9_-]*\.dylib)'
}

bundle_shared_qt_macos() {
	local exe="$1"
	if ! uhdr_links_shared_qt_macos "$exe"; then
		return 0
	fi

	local macdeployqt=""
	if command -v macdeployqt6 &>/dev/null; then
		macdeployqt="$(command -v macdeployqt6)"
	elif command -v macdeployqt &>/dev/null; then
		macdeployqt="$(command -v macdeployqt)"
	fi
	if [[ -z "$macdeployqt" ]]; then
		echo "Warning: uhdr_repack links shared Qt but macdeployqt was not found." >&2
		return 0
	fi

	local staging="$PLUGIN_BIN/.qtdeploy"
	local app="$staging/uhdr_repack.app"
	local plugin_root
	plugin_root="$(dirname "$PLUGIN_BIN")"
	rm -rf "$staging"
	mkdir -p "$app/Contents/MacOS"
	cp "$exe" "$app/Contents/MacOS/uhdr_repack"
	chmod +x "$app/Contents/MacOS/uhdr_repack"

	echo "==> Bundling shared Qt dependencies with macdeployqt"
	"$macdeployqt" "$app" -always-overwrite -no-codesign

	cp "$app/Contents/MacOS/uhdr_repack" "$exe"
	chmod +x "$exe"
	if [[ -d "$app/Contents/Frameworks" ]]; then
		rm -rf "$plugin_root/Frameworks"
		cp -R "$app/Contents/Frameworks" "$plugin_root/"
	fi
	if [[ -d "$app/Contents/PlugIns" ]]; then
		rm -rf "$plugin_root/PlugIns"
		cp -R "$app/Contents/PlugIns" "$plugin_root/"
	fi
	rm -rf "$staging"
}

# macdeployqt rewrites install names after Homebrew's ad-hoc signature. Those
# stale signatures make dyld kill the process (SIGKILL / exit 137) when Lightroom
# launches it. Zip archives that follow symlinks also copy Versions/Current and
# the framework root binary into real files, which codesign rejects as an
# ambiguous bundle. Restore the symlink layout, then sign Mach-O files, framework
# bundles, and the executable.
repair_framework_symlinks() {
	local fw="$1"
	local name ver
	name="$(basename "$fw" .framework)"
	ver="$fw/Versions/A"
	[[ -d "$ver" && -f "$ver/$name" ]] || return 0

	if [[ -e "$fw/Versions/Current" && ! -L "$fw/Versions/Current" ]]; then
		rm -rf "$fw/Versions/Current"
	fi
	if [[ ! -e "$fw/Versions/Current" ]]; then
		ln -s A "$fw/Versions/Current"
	fi

	if [[ -e "$fw/$name" && ! -L "$fw/$name" ]]; then
		rm -f "$fw/$name"
	fi
	if [[ ! -e "$fw/$name" ]]; then
		ln -s "Versions/Current/$name" "$fw/$name"
	fi

	if [[ -e "$ver/Resources" ]]; then
		if [[ -e "$fw/Resources" && ! -L "$fw/Resources" ]]; then
			rm -rf "$fw/Resources"
		fi
		if [[ ! -e "$fw/Resources" ]]; then
			ln -s Versions/Current/Resources "$fw/Resources"
		fi
	fi
}

repair_framework_tree() {
	local dir="$1"
	[[ -d "$dir" ]] || return 0
	local fw
	while IFS= read -r -d '' fw; do
		repair_framework_symlinks "$fw"
	done < <(find "$dir" -name "*.framework" -type d -print0)
}

codesign_macho_tree() {
	local dir="$1"
	[[ -d "$dir" ]] || return 0
	local f
	while IFS= read -r -d '' f; do
		if file -b "$f" | grep -q "Mach-O"; then
			codesign --force --sign - "$f"
		fi
	done < <(find "$dir" -type f -print0)
}

codesign_framework_bundles() {
	local dir="$1"
	[[ -d "$dir" ]] || return 0
	local fw
	# Deepest bundles first so a nested framework is sealed before its parent.
	while IFS= read -r fw; do
		[[ -n "$fw" ]] || continue
		codesign --force --sign - "$fw"
	done < <(find "$dir" -name "*.framework" -type d -print | awk '{ print length($0) "\t" $0 }' | sort -nr | cut -f2-)
}

verify_macos_codesign() {
	local exe="$1"
	local plugin_root="$2"
	local qtcore="$plugin_root/Frameworks/QtCore.framework/Versions/A/QtCore"
	if ! codesign --verify --strict "$exe"; then
		echo "codesign verify failed: $exe" >&2
		exit 1
	fi
	if [[ -f "$qtcore" ]] && ! codesign --verify --strict "$qtcore"; then
		echo "codesign verify failed: $qtcore" >&2
		exit 1
	fi
}

codesign_macos_bundle() {
	echo "==> Ad-hoc codesign (required after copying into the plug-in bundle)"
	local exe="$PLUGIN_BIN/uhdr_repack"
	local plugin_root
	plugin_root="$(dirname "$PLUGIN_BIN")"
	local entitlements="$SCRIPT_DIR/macos/uhdr_repack.entitlements"

	if uhdr_links_shared_qt_macos "$exe"; then
		repair_framework_tree "$plugin_root/Frameworks"
		codesign_macho_tree "$plugin_root/Frameworks"
		codesign_macho_tree "$plugin_root/PlugIns"
		codesign_framework_bundles "$plugin_root/Frameworks"
		if [[ ! -f "$entitlements" ]]; then
			echo "Missing entitlements file: $entitlements" >&2
			exit 1
		fi
		codesign --force --sign - --entitlements "$entitlements" "$exe"
		# Chrome quarantine survives codesign and then dyld refuses the Qt libraries
		# ("library load disallowed by system policy") even when the signature is valid.
		if command -v xattr >/dev/null 2>&1; then
			xattr -dr com.apple.quarantine "$plugin_root" || true
		fi
		verify_macos_codesign "$exe" "$plugin_root"
	else
		codesign --force --sign - "$exe"
		codesign --verify --strict "$exe"
	fi
}

using_shared_qt_windows() {
	[[ "${UHDR_STATIC_QT:-ON}" == "OFF" || "${UHDR_STATIC_QT:-ON}" == "0" ]]
}

find_windeployqt() {
	local candidate=""
	if command -v windeployqt6 &>/dev/null; then
		command -v windeployqt6
		return 0
	fi
	if command -v windeployqt &>/dev/null; then
		command -v windeployqt
		return 0
	fi

	local prefixes=()
	if [[ -n "${QT_ROOT_DIR:-}" ]]; then
		prefixes+=("$QT_ROOT_DIR")
	fi
	if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
		local entry
		IFS=':' read -ra entries <<<"${CMAKE_PREFIX_PATH}"
		for entry in "${entries[@]}"; do
			prefixes+=("$entry")
		done
	fi

	local prefix
	for prefix in "${prefixes[@]}"; do
		for candidate in \
			"$prefix/bin/windeployqt6.exe" \
			"$prefix/bin/windeployqt.exe"; do
			if [[ -f "$candidate" ]]; then
				echo "$candidate"
				return 0
			fi
		done
	done
	return 1
}

uhdr_links_shared_qt_windows() {
	local exe="$1"
	if ! command -v dumpbin &>/dev/null; then
		return 1
	fi
	dumpbin /nologo /dependents "$exe" 2>/dev/null | grep -qi 'Qt[0-9A-Za-z_-]*\.dll'
}

bundle_shared_qt_windows() {
	local exe="$1"
	if ! using_shared_qt_windows; then
		return 0
	fi

	local windeployqt=""
	windeployqt="$(find_windeployqt)" || {
		echo "Warning: shared Qt build but windeployqt was not found." >&2
		return 0
	}

	echo "==> Bundling shared Qt dependencies with windeployqt"
	"$windeployqt" --no-translations --no-compiler-runtime "$exe"
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
	if [[ "${UHDR_REQUIRE_STATIC_QT:-}" == "1" || "${UHDR_REQUIRE_STATIC_QT:-}" == "ON" ]]; then
		if uhdr_links_shared_qt_macos "$PLUGIN_BIN/uhdr_repack"; then
			echo "Release build requires a single uhdr_repack with no shared Qt." >&2
			exit 1
		fi
	fi
	bundle_shared_qt_macos "$PLUGIN_BIN/uhdr_repack"

	# libuhdr and libjpeg-turbo are linked statically; shared Qt is bundled above.
	codesign_macos_bundle

	if [[ "${UHDR_REQUIRE_STATIC_QT:-}" == "1" || "${UHDR_REQUIRE_STATIC_QT:-}" == "ON" ]]; then
		local plugin_root
		plugin_root="$(dirname "$PLUGIN_BIN")"
		if [[ -d "$plugin_root/Frameworks" || -d "$plugin_root/PlugIns" ]]; then
			echo "Static bundle must not contain Frameworks or PlugIns." >&2
			exit 1
		fi
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
	bundle_shared_qt_windows "$PLUGIN_BIN/uhdr_repack.exe"

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
	"$SCRIPT_DIR/copy_ui_fixtures.sh"
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
	if is_windows_host && [[ -n "${GITHUB_ACTIONS:-}" ]]; then
		powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$SCRIPT_DIR/run_uhdr_test.ps1"
		return
	fi
	"$SCRIPT_DIR/run_uhdr_test.sh"
}

cmd_install() {
	cmd_build
	cmd_bundle
	cmd_test
}

cmd_package() {
	"$SCRIPT_DIR/package_plugin.sh" "$(detect_platform_id)"
}

case "$COMMAND" in
install-deps) cmd_install_deps ;;
install) cmd_install ;;
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
