# Changelog

All notable changes to **Ultra HDR Export** are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Each public build is tagged `vX.Y.Z-rN`, where `X.Y.Z` comes from `Info.lua` semver fields and `N` from `VERSION.build`.

## Unreleased

## v1.0.0-r11

### Fixed

- Windows CI configure failure (`nmake` not found / `CMAKE_CXX_COMPILER` unset): bundle script selects a Visual Studio CMake generator instead of defaulting to NMake Makefiles outside a dev shell.

## v1.0.0-r10

### Fixed

- Windows CI turbojpeg configure failure on CMake 4.x: pin CMake **3.31.6** on the Windows release job (libultrahdr vendored libjpeg-turbo 3.0.1 requires CMake 3.x).

### Added

- Docker repro for the CMake 4 policy regression: `scripts/docker/repro-cmake4-turbojpeg/run.sh`.

## v1.0.0-r9

### Fixed

- Windows CI configure failure: enable libultrahdr `UHDR_BUILD_DEPS` on Windows to vendor libjpeg-turbo; remove redundant `find_package(JPEG)` from `uhdr_repack`.

### Changed

- Windows release workflow runs `run_uhdr_test.ps1` after bundling the encoder.

## v1.0.0-r8

### Added

- **Windows x64** support: WIC-based `uhdr_repack.exe` encoder, portable Lua plug-in layer (`uhdr_repack.exe`, `LrFileUtils` file ops), and `scripts/bundle_uhdr_for_plugin_windows.ps1`.
- Windows smoke test: `scripts/run_uhdr_test.ps1`.
- Separate GitHub Release assets per OS: `ExportHDR.lrplugin-macos-arm64.zip` and `ExportHDR.lrplugin-windows-x64.zip` (no mixed binaries in one archive).
- Per-build changelog and release notes sourced from this file.

### Changed

- CI release workflow builds macOS and Windows in parallel, then publishes both zips on one release.
- `uhdr_repack` CMake selects platform-specific image loaders (Core Image on macOS, WIC on Windows) with shared encode/slice/verify code.

## v1.0.0-r7

### Added

- Optional **Slicing** (`1:1` or `4:5`) in the export filter: keeps the full exported height, preserves the original Ultra HDR file, and writes numbered Ultra HDR slice JPEGs next to it (each with its own gain map).

## v1.0.0-r6

### Changed

- Default **max content boost** raised from `100` to `1000`.

### Fixed

- Release zip excludes plug-in `bin/.gitignore` and `bin/README.txt`.

## v1.0.0-r5

### Added

- Initial public release: Lightroom Classic export filter plus bundled `uhdr_repack` for Ultra HDR JPEG output on **macOS 26 (Tahoe), ARM64**.

## v1.0.0-r4

### Fixed

- Vendored **libultrahdr** include path points at the library root so `ultrahdr_api.h` resolves during CI builds.

### Changed

- Early CI packaging for **macOS Apple Silicon (arm64)** on GitHub `macos-14`.
