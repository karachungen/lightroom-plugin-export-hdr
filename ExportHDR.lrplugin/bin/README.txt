Bundled encoder directory (macOS Apple Silicon / arm64 only)

After cloning the repo, this folder is empty of binaries until you run:

  ./scripts/bundle_uhdr_for_plugin.sh

That configures CMake to fetch and build google/libultrahdr (with UHDR_WRITE_XMP)
inside tools/uhdr_repack/build, builds uhdr_repack for arm64, copies it here, and
bundles libuhdr + libjpeg .dylibs next to the binary.

Requires: Xcode CLT, CMake, libjpeg-turbo (e.g. Homebrew: jpeg-turbo).
Optional: brew install dylibbundler (otherwise an otool-based fallback copies deps).

Optional override: UHDR_USE_SYSTEM=1 to link against a preinstalled libultrahdr
(and UHDR_ROOT=... if CMake cannot find headers/libs).

If ./uhdr_repack exits immediately with "killed", the bundle script must re-sign
binaries after rewriting library paths (the script runs codesign --sign -).
Re-run bundle_uhdr_for_plugin.sh from the repo.
