# Changelog

All notable changes to **Ultra HDR Export** are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Each public build is tagged `vX.Y.Z-rN`, where `X.Y.Z` comes from `Info.lua` semver fields and `N` from `VERSION.build`.

## Unreleased

### Added

- Per-build changelog and release notes sourced from this file.

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
