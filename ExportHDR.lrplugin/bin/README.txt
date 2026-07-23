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
logic as GitHub Actions. They build google/libultrahdr and libjpeg-turbo (both vendored
via FetchContent, both linked statically) inside tools/uhdr_repack/build, then copy the
single self-contained encoder here:

  macOS:  bin/uhdr_repack
  Windows: bin/uhdr_repack.exe

GitHub Releases ship separate archives (no mixed OS binaries in one zip):
  ExportHDR.lrplugin-macos-arm64.zip
  ExportHDR.lrplugin-windows-x64.zip

macOS requires: Xcode CLT, CMake. uhdr_repack links libuhdr and libjpeg-turbo statically,
so nothing else needs to be installed or bundled at runtime.

Windows requires: Git, CMake 3.31.x, Ninja, and MSVC (Visual Studio 2022 Build Tools, x64).
One-time setup from repo root:

  .\scripts\setup_windows_build.ps1

(MSVC install prompts for Administrator.) Then:

  .\scripts\build_plugin.ps1 all

libjpeg-turbo is built automatically via libultrahdr (UHDR_BUILD_DEPS) during configure.
CMake 4.x rejects vendored libjpeg-turbo 3.0.1's old cmake_minimum_required(); build_plugin.sh
works around this on both platforms by exporting CMAKE_POLICY_VERSION_MINIMUM=3.5. The
Windows CMake 3.31.x pin above predates that workaround and has not been re-verified against
CMake 4.x — treat it as still required until someone confirms otherwise on Windows.

Optional override: UHDR_USE_SYSTEM=1 to link against a preinstalled libultrahdr
(and UHDR_ROOT=... if CMake cannot find headers/libs).
