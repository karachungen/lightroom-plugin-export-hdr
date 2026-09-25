# Sourced by build_plugin.sh and scripts/test_qt_kit.sh.
# Caller defines qt_install_kit PLATFORM PREFIX STAMP_FILE.

qt_kit_root() {
	cd "${BASH_SOURCE[0]%/*}/.." && pwd
}

qt_cache_dir() {
	local root
	root="$(qt_kit_root)"
	echo "${UHDR_BUILD_CACHE_DIR:-$root/.build-cache}"
}

qt_read_stamp() {
	local file="$1" line key value
	[[ -f "$file" ]] || { echo "Missing Qt stamp: $file" >&2; exit 1; }
	QT_STAMP_VERSION=""
	QT_STAMP_LINKAGE=""
	QT_STAMP_ARCH=""
	QT_STAMP_DEPLOYMENT_TARGET=""
	QT_STAMP_SUBMODULES=""
	QT_STAMP_AQT_ARCH=""
	QT_STAMP_MODULES=""
	while IFS= read -r line || [[ -n "$line" ]]; do
		[[ -z "$line" || "$line" == \#* ]] && continue
		key="${line%%=*}"
		value="${line#*=}"
		case "$key" in
		version) QT_STAMP_VERSION="$value" ;;
		linkage) QT_STAMP_LINKAGE="$value" ;;
		arch) QT_STAMP_ARCH="$value" ;;
		deployment_target) QT_STAMP_DEPLOYMENT_TARGET="$value" ;;
		submodules) QT_STAMP_SUBMODULES="$value" ;;
		aqt_arch) QT_STAMP_AQT_ARCH="$value" ;;
		modules) QT_STAMP_MODULES="$value" ;;
		*) echo "Unknown stamp key $key in $file" >&2; exit 1 ;;
		esac
	done <"$file"
	local required=(version linkage arch)
	if [[ "$QT_STAMP_LINKAGE" == "static" ]]; then
		required+=(deployment_target submodules)
	elif [[ "$QT_STAMP_LINKAGE" == "shared" ]]; then
		required+=(aqt_arch modules)
	else
		echo "Stamp $file has unsupported linkage '${QT_STAMP_LINKAGE}'" >&2
		exit 1
	fi
	local req
	for req in "${required[@]}"; do
		local var="QT_STAMP_${req^^}"
		[[ -n "${!var}" ]] || { echo "Stamp $file missing $req" >&2; exit 1; }
	done
}

qt_prefix_path() {
	local platform="$1" stamp
	stamp="$(qt_kit_root)/scripts/qt/${platform}.stamp"
	qt_read_stamp "$stamp"
	case "$platform" in
	macos-arm64)
		echo "${QT_STATIC_ROOT:-$HOME/Qt/${QT_STAMP_VERSION}-static}"
		;;
	windows-x64)
		if [[ -n "${QT_ROOT_DIR:-}" ]]; then
			echo "$QT_ROOT_DIR"
		else
			echo "C:/Qt/${QT_STAMP_VERSION}/msvc2022_64"
		fi
		;;
	*)
		echo "Unknown Qt platform: $platform" >&2
		exit 1
		;;
	esac
}

qt_archive_path() {
	echo "$(qt_cache_dir)/qt-$1.tar.zst"
}

qt_query_prefix_version() {
	local prefix="$1" qmake
	for qmake in "$prefix/bin/qmake6" "$prefix/bin/qmake" "$prefix/bin/qmake.exe"; do
		if [[ -x "$qmake" || -f "$qmake" ]]; then
			"$qmake" -query QT_VERSION 2>/dev/null && return 0
		fi
	done
	return 1
}

qt_require_zstd() {
	if ! command -v zstd >/dev/null 2>&1; then
		echo "zstd is required to pack the Qt kit. Run ./scripts/build_plugin.sh install-deps" >&2
		exit 1
	fi
}

qt_write_archive() {
	local prefix="$1" archive="$2"
	qt_require_zstd
	mkdir -p "$(dirname "$archive")"
	tar -C "$prefix" -cf - . | zstd -T0 -o "$archive"
}

qt_extract_archive() {
	local archive="$1" prefix="$2"
	qt_require_zstd
	mkdir -p "$prefix"
	zstd -d -c "$archive" | tar -C "$prefix" -xf -
}

qt_delete_prefix() {
	local platform="$1" prefix="$2" version_dir
	if [[ "$platform" == "windows-x64" ]]; then
		version_dir="$(dirname "$prefix")"
		if [[ "$(basename "$version_dir")" == "$QT_STAMP_VERSION" ]]; then
			rm -rf "$version_dir"
			return 0
		fi
	fi
	rm -rf "$prefix"
}

qt_prefix_ok() {
	local prefix="$1" stamp="$2" version
	[[ -f "$prefix/lib/cmake/Qt6/Qt6Config.cmake" ]] || return 1
	[[ -f "$prefix/.uhdr-qt-stamp" ]] || return 1
	cmp -s "$prefix/.uhdr-qt-stamp" "$stamp" || return 1
	version="$(qt_query_prefix_version "$prefix" || true)"
	[[ "$version" == "$QT_STAMP_VERSION" ]]
}

ensure_qt_kit() {
	local platform="$1"
	local root stamp prefix archive config version
	root="$(qt_kit_root)"
	stamp="$root/scripts/qt/${platform}.stamp"
	qt_read_stamp "$stamp"
	if [[ "$platform" == "macos-arm64" && "$QT_STAMP_LINKAGE" != "static" ]]; then
		echo "macOS stamp must be linkage=static" >&2
		exit 1
	fi
	if [[ "$platform" == "windows-x64" && "$QT_STAMP_LINKAGE" != "shared" ]]; then
		echo "Windows stamp must be linkage=shared" >&2
		exit 1
	fi
	prefix="$(qt_prefix_path "$platform")"
	archive="$(qt_archive_path "$platform")"
	config="$prefix/lib/cmake/Qt6/Qt6Config.cmake"

	if [[ -f "$config" && -f "$prefix/.uhdr-qt-stamp" ]] && cmp -s "$prefix/.uhdr-qt-stamp" "$stamp"; then
		echo "==> Qt kit ready at $prefix"
		return 0
	fi

	if [[ -f "$config" && ! -f "$prefix/.uhdr-qt-stamp" ]]; then
		version="$(qt_query_prefix_version "$prefix" || true)"
		if [[ "$version" == "$QT_STAMP_VERSION" ]]; then
			cp "$stamp" "$prefix/.uhdr-qt-stamp"
			if [[ ! -f "$archive" ]]; then
				qt_write_archive "$prefix" "$archive"
			fi
			echo "==> Qt kit ready at $prefix"
			return 0
		fi
	fi

	if [[ -f "$archive" ]]; then
		qt_delete_prefix "$platform" "$prefix"
		mkdir -p "$prefix"
		qt_extract_archive "$archive" "$prefix"
		if qt_prefix_ok "$prefix" "$stamp"; then
			echo "==> Extracting Qt kit from $archive"
			return 0
		fi
		if [[ "${UHDR_QT_CACHE_RESTORED:-}" == "true" ]]; then
			echo "Restored Qt archive failed the prefix check: $archive" >&2
			echo "Edit scripts/qt/${platform}.stamp so the Actions cache key changes, then re-run." >&2
			exit 1
		fi
		rm -f "$archive"
		qt_delete_prefix "$platform" "$prefix"
	fi

	if ! declare -F qt_install_kit >/dev/null; then
		echo "qt_install_kit is not defined" >&2
		exit 1
	fi
	qt_delete_prefix "$platform" "$prefix"
	qt_install_kit "$platform" "$prefix" "$stamp"
	if ! qt_prefix_ok "$prefix" "$stamp"; then
		# Installer does not write the marker. Check version, then we write it.
		version="$(qt_query_prefix_version "$prefix" || true)"
		if [[ ! -f "$config" || "$version" != "$QT_STAMP_VERSION" ]]; then
			echo "Qt install at $prefix failed the prefix check (version ${version:-missing}, stamp $QT_STAMP_VERSION)." >&2
			exit 1
		fi
		cp "$stamp" "$prefix/.uhdr-qt-stamp"
	fi
	qt_write_archive "$prefix" "$archive"
	echo "==> Qt kit ready at $prefix"
}
