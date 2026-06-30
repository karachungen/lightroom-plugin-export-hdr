Bundled encoder directory (platform-specific binaries)

After cloning the repo, this folder is empty of binaries until you build for your OS:

  macOS (26 Tahoe, ARM64):
    ./scripts/build_plugin.sh

  Windows (x64):
    .\scripts\build_plugin.ps1

  Legacy (build + bundle only, no test/zip):
    ./scripts/bundle_uhdr_for_plugin.sh
    .\scripts\bundle_uhdr_for_plugin_windows.ps1

The build scripts use CMake presets (tools/uhdr_repack/CMakePresets.json) — the same
logic as GitHub Actions. They build google/libultrahdr (vendored via FetchContent) inside
tools/uhdr_repack/build, copy the encoder here, and bundle runtime libraries next to
the binary:

  macOS:  bin/uhdr_repack + *.dylib
  Windows: bin/uhdr_repack.exe + *.dll

GitHub Releases ship separate archives (no mixed OS binaries in one zip):
  ExportHDR.lrplugin-macos-arm64.zip
  ExportHDR.lrplugin-windows-x64.zip

macOS requires: Xcode CLT, CMake, libjpeg-turbo (e.g. Homebrew: jpeg-turbo).
Optional on macOS: brew install dylibbundler (otherwise an otool-based fallback copies deps).

Windows requires: Git, CMake 3.31.x, Ninja, and MSVC (Visual Studio 2022 Build Tools, x64).
One-time setup from repo root:

  .\scripts\setup_windows_build.ps1

(MSVC install prompts for Administrator.) Then:

  .\scripts\build_plugin.ps1 all

libjpeg-turbo is built automatically via libultrahdr (UHDR_BUILD_DEPS) during configure.
CMake 4.x currently breaks vendored libjpeg-turbo 3.0.1; pin CMake 3.31.x on Windows.

Optional override: UHDR_USE_SYSTEM=1 to link against a preinstalled libultrahdr
(and UHDR_ROOT=... if CMake cannot find headers/libs).

macOS only: if ./uhdr_repack exits immediately with "killed", re-run the build
(it ad-hoc re-signs after rewriting library paths).
