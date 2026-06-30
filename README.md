# lightroom-plugin-export-hdr

Lightroom Classic **export filter** for **macOS 26 (Tahoe), ARM64** and **Windows x64**, plus **`uhdr_repack`**: one Ultra HDR (gain-map) **JPEG** from your normal export and an internal HDR TIFF, using [google/libultrahdr](https://github.com/google/libultrahdr).

## What it does

- Keeps **Image Sizing** and most export options for the main pass (SDR base, often JPEG).
- Runs a **second internal export** → temporary **HDR TIFF**, then **`ExportHDR.lrplugin/bin/uhdr_repack`** (or **`uhdr_repack.exe`** on Windows) merges into one **`.jpg`** (Ultra HDR).
- Optional **Slicing** (`1:1` or `4:5`) keeps the full exported height, preserves the original Ultra HDR file, and writes numbered Ultra HDR slice JPEGs next to it (each with its own gain map).
- **Lightroom Classic 14+** — `[Info.lua](ExportHDR.lrplugin/Info.lua)` (`LrSdkMinimumVersion = 14.0`). Export filter: **Encode Ultra HDR JPEG (uhdr_repack)** (under plug-in **Ultra HDR Export**).

## How it works

```mermaid
flowchart TD
  subgraph lightroom [What Lightroom does]
    start["You run Export with the filter enabled"]
    p1["Pass 1: normal export session"]
    p2["Pass 2: second internal export session"]
    start --> p1
    start --> p2
    p1 -->|"Renders at your Image Sizing; keeps format you chose"| sdr["SDR base file on disk e.g. JPEG"]
    p2 -->|"Forces HDR TIFF Rec2020 32-bit for this pass only"| hdr["Temporary HDR TIFF same pixel size"]
  end

  subgraph helper ["uhdr_repack inside ExportHDR.lrplugin/bin"]
    load["Loads HDR TIFF and SDR base paths"]
    run["libultrahdr builds primary JPEG plus embedded HDR gain map"]
    load --> run
  end

  sdr --> load
  hdr --> load
  run -->|"Writes to the same path as the SDR base"| final["One Ultra HDR JPEG replaces the base file"]
```



1. **Pass 1** — **SDR base** (your File Settings format, usually JPEG) at destination, using your sizing and edits.
2. **Pass 2** — Same settings, extra export job → **32-bit HDR TIFF** (wide gamut / HDR display), same pixel size.
3. `**uhdr_repack`** — `**--hdr-tiff`**, `**--base`**, `**--out**` replaces the base path with the merged Ultra HDR JPEG.
4. **Logs** — `**uhdr_export_<timestamp>.log`** next to the export folder. **Save debug copies** (optional) → `**_uhdr_sdr`** / `**_uhdr_hdr`** before overwrite.

## How to use

1. **Bundle** — Build the encoder for your OS (binaries are gitignored):
   - **macOS:** `./scripts/build_plugin.sh` → `ExportHDR.lrplugin/bin/uhdr_repack` + `.dylib`
   - **Windows:** `.\scripts\build_plugin.ps1` → `ExportHDR.lrplugin\bin\uhdr_repack.exe` + `.dll`
   - Legacy aliases: `bundle_uhdr_for_plugin.sh` / `bundle_uhdr_for_plugin_windows.ps1` (build + bundle only)
   - Pre-built zips ship separately from [GitHub Releases](https://github.com/karachungen/lightroom-plugin-export-hdr/releases):
     - `ExportHDR.lrplugin-macos-arm64.zip`
     - `ExportHDR.lrplugin-windows-x64.zip`
2. **Install** — **File → Plug-in Manager → Add** → `[ExportHDR.lrplugin](ExportHDR.lrplugin)`
3. **Export** — Set **Destination** and **Image Sizing** as usual, then wire up the filter:
  - In the Export dialog, open **Post-Process Actions** (wording may vary slightly by Lightroom version; same area as post-processing / export filters).
  - Click **Add** and choose **Ultra HDR Export** → **Encode Ultra HDR JPEG (uhdr_repack)**.
  - If Lightroom shows **Install** for that action, click it so the plug-in’s options load for this export.
  - Expand or select the new action so the **Encode Ultra HDR JPEG (uhdr_repack)** section appears; adjust **Base quality**, **Gain map Q**, **Slicing**, and other options there.
  - For `**.jpg`**, use **JPEG** in **File Settings**; do not use a manual 32-bit HDR TIFF as the main file.
4. **Develop** — **HDR** mode when the photo supports it.

## Build locally

Local builds use the same orchestrator as [GitHub Actions](.github/workflows/release-plugin.yml): CMake presets in `tools/uhdr_repack/CMakePresets.json`, then bundle, smoke test, and zip.

**macOS (ARM64)** — from repo root:

```bash
./scripts/build_plugin.sh install-deps   # once: brew install cmake jpeg-turbo dylibbundler
./scripts/build_plugin.sh all            # build → bundle → test → package
```

**Windows (x64)** — from repo root in PowerShell:

```powershell
# One-time: Git, CMake 3.31.6, Ninja, VS 2022 Build Tools (MSVC x64)
.\scripts\setup_windows_build.ps1

# Same pipeline as CI
.\scripts\build_plugin.ps1 all
```

Or install deps automatically on first build:

```powershell
.\scripts\build_plugin.ps1 -InstallDeps all
```

Individual steps: `build`, `bundle`, `test`, `package` (see `./scripts/build_plugin.sh --help`).

CMake **FetchContent** → `tools/uhdr_repack/build/_deps/` · **libjpeg** via **FindJPEG** (e.g. Homebrew **jpeg-turbo** on macOS) · optional `UHDR_USE_SYSTEM=1`, `UHDR_ROOT=...` · macOS ships **codesign** for `bin/uhdr_repack` and `bin/*.dylib`

**Encoder only** — from `tools/uhdr_repack` (same presets as the plug-in build):

```bash
cmake --preset macos-arm64-release -S tools/uhdr_repack
cmake --build tools/uhdr_repack/build
```

```powershell
cmake --preset windows-x64-release -S tools/uhdr_repack
cmake --build tools/uhdr_repack/build
```

→ `build/uhdr_repack` (macOS) or `build/uhdr_repack.exe` (Windows, Ninja) · flags & `--inspect`: [tools/uhdr_repack/README.md](tools/uhdr_repack/README.md)