Bundled encoder directory (platform-specific binaries)

After cloning the repo, this folder is empty of binaries until you run the bundle script for your OS:

  macOS (26 Tahoe, ARM64):
    ./scripts/bundle_uhdr_for_plugin.sh

  Windows (x64):
    .\scripts\bundle_uhdr_for_plugin_windows.ps1

Each script builds google/libultrahdr (vendored via CMake FetchContent) inside
tools/uhdr_repack/build, copies the encoder here, and bundles runtime libraries
next to the binary:

  macOS:  bin/uhdr_repack + *.dylib
  Windows: bin/uhdr_repack.exe + *.dll

GitHub Releases ship separate archives (no mixed OS binaries in one zip):
  ExportHDR.lrplugin-macos-arm64.zip
  ExportHDR.lrplugin-windows-x64.zip

macOS requires: Xcode CLT, CMake, libjpeg-turbo (e.g. Homebrew: jpeg-turbo).
Optional on macOS: brew install dylibbundler (otherwise an otool-based fallback copies deps).

Windows requires: CMake, MSVC Build Tools, and a JPEG library CMake can find.

Optional override: UHDR_USE_SYSTEM=1 to link against a preinstalled libultrahdr
(and UHDR_ROOT=... if CMake cannot find headers/libs).

macOS only: if ./uhdr_repack exits immediately with "killed", re-run the bundle
script (it ad-hoc re-signs after rewriting library paths).
