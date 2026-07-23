# Changelog

All notable changes to **Ultra HDR Export** are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Each public release is tagged `vX.Y.Z`, where `X.Y.Z` comes from `Info.lua` `major` / `minor` / `revision`. Older releases used a `-rN` build suffix (see historical sections below).

## Unreleased

## v2.0.6

### Changed

- Ship a single self-contained `uhdr_repack` binary on macOS and Windows: libultrahdr and libjpeg-turbo are linked statically, so downloads no longer include separate `.dylib` / `.dll` files that Gatekeeper can block one-by-one (closes #3).

### Removed

- macOS local-build requirement for Homebrew `jpeg-turbo` and `dylibbundler` (jpeg is vendored via `UHDR_BUILD_DEPS`).

## v2.0.5

### Fixed

- macOS export filter: quote the bundled `uhdr_repack` path in `CMD.shellBinary` so installs under `~/Library/Application Support/Adobe/Lightroom/Modules` (and other paths with spaces) no longer fail with `sh: …/Application: No such file or directory` (issue #2).

## v2.0.4

### Fixed

- Windows export filter: encoding always failed with `uhdr_repack failed (exit 1, raw 1)` because `LrTasks.execute` passes the line to `cmd /c` verbatim and cmd strips the first and last quote when the line starts with a quoted exe path. `runShell` now wraps the whole command in sacrificial outer quotes.
- Windows export filter: `--inspect` verification (`inspectIsUltraHdr`) ran a relative `uhdr_repack.exe` via `io.popen` from Lightroom's working directory, so the encoder was never found; it now resolves the bundled absolute path and uses the same sacrificial-quote wrapping.
- Windows quoting regression test invoked cmd via `Start-Process`, which re-quotes the argument string and masked the bug; it now passes the command line to `cmd.exe /c` verbatim like `LrTasks.execute`, asserts the unwrapped pattern fails, and checks `--inspect` output. The test now runs in the Windows release CI job (it previously existed only as a manual script).

## v2.0.3

### Fixed

- macOS export filter: stop using `package.config` for platform detection; Lightroom's Lua sandbox does not expose `package`, which caused an internal error on `Command.lua` line 20.
- macOS export filter: final JPEG stayed SDR-only after v2.0.1 because staged Ultra HDR was not promoted to the Lightroom export path; restore direct `--out` on macOS (Windows keeps temp staging with delete-before-copy verification).
- macOS export filter: fail export when `--inspect` reports the final JPEG is not Ultra HDR (catches silent SDR-only promotion).

### Changed

- Local build default is `install` (build → bundle → test, updates `ExportHDR.lrplugin` in place, no zip); CI/release still uses `all`.
- `package_plugin.sh` refuses to zip when `Info.lua` is missing (prevents bin-only release archives).

## v2.0.2

### Fixed

- Windows export filter: stop using `cmd /c cd /d … && uhdr_repack.exe` (double cmd layer broke paths with parentheses); invoke the encoder via a single quoted absolute path so DLLs load from the plug-in `bin` folder without `cd`.

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
