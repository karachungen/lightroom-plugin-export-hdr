# uhdr_repack

macOS command-line tool that builds an **Ultra HDR JPEG** (`.jpg` with embedded HDR gain map, ISO 21496-1 / Adobe `hdrgm` metadata) from:

- A **Lightroom HDR TIFF** export (wide-gamut / PQ / HLG / extended dynamic range as written by Lightroom).
- A matching **SDR base** image (typically JPEG from Lightroom or TIFF).

Pipeline:

1. Core Image loads the HDR TIFF with `expandToHDR` and renders **linear extended BT.2020** RGBA float → **RGBA half-float** for libultrahdr (`UHDR_IMG_FMT_64bppRGBAHalfFloat`, `UHDR_CT_LINEAR`, `UHDR_CG_BT_2100`).
2. Core Image loads the SDR base, scales to match resolution, renders **RGBA8**, then the tool converts to **BT.709 full-range YCbCr 4:2:0** planar (`UHDR_IMG_FMT_12bppYCbCr420`, BT.709 / sRGB) so the primary JPEG is encoded as **4:2:0**.
3. [google/libultrahdr](https://github.com/google/libultrahdr) is built **in-tree** via CMake **FetchContent** (pinned tag `v1.4.0`) with **`UHDR_WRITE_XMP=ON`** so the output includes primary XMP gain-map metadata (`GContainer` / `hdrgm`). It computes and embeds the gain map into a single JPEG.

## Requirements

- macOS (Core Image HDR ingest).
- CMake 3.15+, C++17, Objective-C++.
- **libjpeg-turbo** (or compatible libjpeg) via CMake `FindJPEG` — typically Homebrew `jpeg-turbo`.

You do **not** need a separate Homebrew install of libultrahdr unless you pass **`-DUHDR_USE_SYSTEM=ON`** to CMake.

## Build

```bash
cd tools/uhdr_repack
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Binary: `build/uhdr_repack`. First configure downloads and builds libultrahdr under `build/_deps/`.

Advanced (link against your own installed libultrahdr):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUHDR_USE_SYSTEM=ON -DUHDR_ROOT=/usr/local
cmake --build build
```

### Bundle for Lightroom plug-in (Apple Silicon arm64)

```bash
# From repository root:
./scripts/bundle_uhdr_for_plugin.sh
```

This configures CMake with **`-DCMAKE_OSX_ARCHITECTURES=arm64`** and **`CMAKE_OSX_DEPLOYMENT_TARGET=13.0`**, builds, copies `uhdr_repack` to **`ExportHDR.lrplugin/bin/`**, copies **`libuhdr*.dylib`** from the FetchContent build, rewrites `@rpath` to **`@loader_path`**, then bundles **libjpeg** (and other deps). Optional: `brew install dylibbundler` for the most reliable dependency copying.

Intel (x86_64) Macs are **not** covered by that script; build `uhdr_repack` manually for x86_64 and place it at **`ExportHDR.lrplugin/bin/uhdr_repack`** if you maintain a custom Intel bundle.

## Lightroom Classic plug-in (automated)

The repo includes **`ExportHDR.lrplugin`** (macOS **arm64** bundle path, Lightroom Classic **13+**). It adds an **export filter** that:

1. Uses your normal export (SDR base), preserving **Image Sizing**.
2. Renders a temporary **HDR TIFF** with matching dimensions.
3. Invokes **`ExportHDR.lrplugin/bin/uhdr_repack`** (after running **`scripts/bundle_uhdr_for_plugin.sh**`) to replace the exported file with an Ultra HDR JPEG.

Install the bundle via **Plug-in Manager**, run the bundle script so **`bin/uhdr_repack`** exists, then enable **Ultra HDR (uhdr_repack)** in the export filters / post-processing section. See the [repository README](../../README.md) for setup, **codesigning**, troubleshooting, and release notes.

## Usage

```bash
./uhdr_repack --hdr-tiff export_hdr.tif --base export_sdr.jpg --out output_uhdr.jpg
```

Optional tuning:

```text
--base-quality 92
--gainmap-quality 85
--gainmap-scale 1
--min-content-boost 1.0
--max-content-boost 100
--target-display-peak 1000
--monochrome-gainmap
```

Default **`--gainmap-scale`** is **1** (gain map same pixel size as the base image).

Inspect a JPEG:

```bash
./uhdr_repack --inspect output_uhdr.jpg
```

Reports dimensions, libultrahdr recognition, rough JPEG subsampling (4:2:0 heuristic), and marker hints.

## Lightroom export notes

- Export **HDR TIFF** with HDR output enabled; disable **Maximum Compatibility** if you follow workflows from third-party HDR gain-map tools.
- Export **SDR base** from a virtual copy or adjusted edit so creative intent matches the HDR edit.
- Match pixel dimensions (the tool scales the SDR export to the HDR TIFF size).

## Chroma subsampling (4:2:0)

The **SDR base** path supplies **YCbCr 4:2:0** to libultrahdr, so the embedded primary JPEG is typically **4:2:0** (`--inspect` → `primary_jpeg_420: likely`). The **gain map** JPEG may be multi-channel **4:4:4** or **single-channel grayscale** (`--monochrome-gainmap`); grayscale has no chroma subsampling in the usual sense.

## Automated test (`scripts/run_uhdr_test.sh`)

Place **`test/hdr-raw.tif`** and **`test/sdr.jpg`** (see `test/README.md`). From the repo root:

```bash
./scripts/run_uhdr_test.sh
```

Encodes with defaults, runs **`--inspect`**, and checks that **`gainmap_size`** matches **`dimensions`** and that **`primary_xmp`** is present.

## References

- [chemharuka/toGainMapHDR](https://github.com/chemharuka/toGainMapHDR) — Core Image HDR load / tone-map patterns.
- [fengshenx/LR_GainMap_HDR_Export_Plugin](https://github.com/fengshenx/LR_GainMap_HDR_Export_Plugin) — Lightroom export hook calling `toGainMapHDR` (different defaults; this tool targets Ultra HDR via libultrahdr).
