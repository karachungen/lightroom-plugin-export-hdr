# Changelog

All notable changes to **Ultra HDR Export** are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Each public release is tagged `vX.Y.Z`, where `X.Y.Z` comes from `Info.lua` `major` / `minor` / `revision`. Older releases used a `-rN` build suffix (see historical sections below).

## Unreleased

## v2.0.1

### Fixed

- Windows export filter: fix nested `cmd /c` quoting so `uhdr_repack.exe` runs when the plug-in lives under paths with spaces or parentheses (e.g. `Downloads\… (1)`); stage `--out` through ASCII temp before copying to the final export path (including numbered slices).

## v2.0.0

### Added

- 🪟 **Windows x64** — WIC-based `uhdr_repack.exe`, portable plug-in layer, and `ExportHDR.lrplugin-windows-x64.zip` release asset.
- ✂️ **Slice images** — optional `1:1` or `4:5` slicing in the export filter; numbered Ultra HDR JPEGs with per-slice gain maps.
- 🛠️ Unified build orchestrator (`scripts/build_plugin.sh` / `build_plugin.ps1`) for local + CI.

### Changed

- 🚀 Major version **2.0.0** — Windows + slicing are now first-class, documented features.

## v1.0.0-r19

### Fixed

- Windows smoke test slice glob: wrap `Get-ChildItem` in `@()` so `.Count` works when exactly one slice file is returned.

## v1.0.0-r18

### Fixed

- Windows smoke test: parse `--inspect` lines with `[regex]::Match` instead of `Select-String.Matches` (pwsh compatibility).

## v1.0.0-r17

### Fixed

- Windows smoke test inspect parsing: join `--inspect` output to a string before regex checks (pwsh returns string arrays).

## v1.0.0-r16

### Fixed

- Windows smoke test: prefer bundled `ExportHDR.lrplugin/bin/uhdr_repack.exe` (DLLs colocated) and handle native exit codes under pwsh 7.

## v1.0.0-r15

### Fixed

- Windows bundle script exit code: explicitly `exit 0` after success so pwsh does not propagate the usage smoke test's exit code 1.

## v1.0.0-r14

### Fixed

- Windows bundle script false failure: disable `$PSNativeCommandUseErrorActionPreference` so the usage smoke test (expected exit code 1) does not abort the script on pwsh 7+.

## v1.0.0-r13

### Fixed

- Windows MSVC build of `wic_utils.cpp`: accept `std::string` error messages and create `IWICBitmapScaler` before scale/crop.

## v1.0.0-r12

### Fixed

- Windows CI on `windows-latest` (VS 2026): activate MSVC via `ilammy/msvc-dev-cmd` and configure with **Ninja** under pinned CMake 3.31.6 (VS 18 generator requires CMake 4.2+, which breaks vendored libjpeg-turbo).

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
