# lightroom-plugin-export-hdr

This repository provides:

- `**tools/uhdr_repack**` — a macOS CLI that turns a **Lightroom HDR TIFF** plus an **SDR base** into a single `**.jpg`** with an embedded HDR gain map (Ultra HDR / gain-map JPEG) using [google/libultrahdr](https://github.com/google/libultrahdr).
- `**ExportHDR.lrplugin**` — a **Lightroom Classic** (macOS) **export filter** that automates the two renders and calls `uhdr_repack`, while keeping Lightroom’s built-in **Image Sizing** and most export options for both passes.

The plug-in loads `**uhdr_repack` from inside the bundle**: `ExportHDR.lrplugin/bin/uhdr_repack` (**Apple Silicon / arm64 only** for the shipped workflow). Populate that folder with `[scripts/bundle_uhdr_for_plugin.sh](scripts/bundle_uhdr_for_plugin.sh)`.

Encoder build details: [tools/uhdr_repack/README.md](tools/uhdr_repack/README.md).

---

## Quick start (Lightroom Classic on macOS, Apple Silicon)

1. **Build and bundle the encoder into the plug-in** (arm64, macOS 13+ deployment target):
  ```bash
   ./scripts/bundle_uhdr_for_plugin.sh
  ```
   CMake **FetchContent** builds [google/libultrahdr](https://github.com/google/libultrahdr) (pinned `v1.4.0`) with **`UHDR_WRITE_XMP`** under `tools/uhdr_repack/build/_deps/` — no separate Homebrew libultrahdr install is required.  
   Optional: `brew install dylibbundler` so dependency `.dylib` files are copied reliably next to `uhdr_repack`.  
   Optional: `UHDR_USE_SYSTEM=1` (and `UHDR_ROOT=...` if needed) to link against a preinstalled libultrahdr instead of the vendored build.
   This writes `**ExportHDR.lrplugin/bin/uhdr_repack**` (and bundled `.dylib` dependencies, including **`libuhdr`**). Those artifacts are **gitignored**; run the script after clone or before shipping a zip.
2. **Install the plug-in**
  Copy or symlink `**ExportHDR.lrplugin`** into Lightroom’s plug-ins folder, or use **File → Plug-in Manager → Add**, and select the `**ExportHDR.lrplugin`** bundle.
3. **Export**
  Open **Export**, configure destination and **Image Sizing** as usual (that sizing is reused for the auxiliary HDR TIFF pass).  
   In the export dialog, add an **export filter** / **post-processing** step (**Ultra HDR (uhdr_repack)** — exact section name varies slightly by Lightroom version), enable it, and adjust options as needed. The plug-in always uses the bundled `**bin/uhdr_repack`**. **Gain map scale** defaults to **1** (full-resolution gain map).
4. **Requirements**
  - **macOS on Apple Silicon (arm64)** for the default bundled binary path. Intel Macs are not supported by this bundle script (you would need a custom fork that bundles an x86_64 `uhdr_repack`).
  - **Lightroom Classic 13+** with **HDR** editing/export support for the auxiliary HDR TIFF step.
  - **Creative Cloud / non–Mac App Store** install recommended so `LrTasks.execute` can run the helper (sandboxed App Store Lightroom may block arbitrary helpers).

---

## How it works

1. Lightroom renders your **normal export** (the **SDR base** — typically JPEG, matching your sizing).
2. The plug-in runs a second internal `**LrExportSession`** with the **same export settings**, only overriding format/destination for a temporary **HDR TIFF** (`Rec2020_hdr`, 32-bit, HDR display enabled).
3. `**uhdr_repack`** runs: `--hdr-tiff` + `--base` + `--out`, writing the **Ultra HDR JPEG** **over the exported base file** at the same path.

Session logs are written **`uhdr_export_<timestamp>.log` in the same folder as each exported file** (the parent directory of the base JPEG path), not only under the top-level **Export To** folder. If you use **Put in Subfolder**, the log sits next to the image inside that subfolder. If a single batch writes to more than one folder, an additional log file `uhdr_export_<timestamp>_2.log` (etc.) is created in the other folder.

**Save debug copies** (export filter checkbox): when enabled, the plug-in copies the SDR base JPEG to `<name>_uhdr_sdr.<ext>` and the internal HDR TIFF to `<name>_uhdr_hdr.tif` next to the output **before** `uhdr_repack` overwrites the base file, and adds extra lines to the log (export prefix, formatted dimensions). Encoder stderr is always appended to the same log file as the encode step.

---

## File Settings (what to pick)

- **Use JPEG** for the main **File Settings** format when you want a typical Ultra HDR **`.jpg`** result. Lightroom’s first render is the **SDR base**; the plug-in overwrites that file with the Ultra HDR JPEG from `uhdr_repack`.
- **You do not** set **TIFF + HDR Output + 32-bit** in **File Settings** to “create the HDR side” yourself. The plug-in runs a **second internal export** that produces the **HDR TIFF** (see `mergeHdrTiffSettings` in the plug-in) using the same **Image Sizing** as your dialog.
- **Avoid** making the main export a **32-bit HDR TIFF** unless you know you need that workflow. That path produces an HDR file as the “base” render, which is a different model than a JPEG SDR base + internal HDR TIFF.
- For **Develop**, use **HDR** editing when the image supports it (Lightroom Classic 13+).
- More detail: in the **Export** dialog, open the **Ultra HDR (uhdr_repack)** filter section; the **How to use** text at the top matches this section.

---

## Troubleshooting


| Issue                              | What to try                                                                                                                                                                                                |
| ---------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Could not load post-processing filter** | Lightroom only allows `import` for built-in `Lr*` APIs. This plug-in loads `UHDRSettings.lua`, `Command.lua`, etc. via `loadfile` from `PluginInit.lua`. Pull the latest repo, then **Plug-in Manager** → reload or re-add `ExportHDR.lrplugin`. Use Lightroom Classic **13+**. |
| **HDR TIFF render failed**         | Edit the photo in **HDR** mode in Develop; use **Lr 13+**. Check that an HDR TIFF export works manually from the Export dialog.                                                                            |
| **uhdr_repack not found**          | Run `./scripts/bundle_uhdr_for_plugin.sh` so `ExportHDR.lrplugin/bin/uhdr_repack` exists inside the bundle.                                                                                               |
| **Wrong architecture**             | Bundled binary is **arm64**. On Intel Macs this repo’s bundle script does not apply; you need a matching `uhdr_repack` inside `bin/` (custom packaging).                                                                                  |
| **Missing dylibs on another Mac**  | Re-run the bundle script with **dylibbundler** installed; ensure all `*.dylib` next to `uhdr_repack` are included when zipping the `.lrplugin`.                                                            |
| **Encoder won’t run / Gatekeeper** | Codesign the bundled `uhdr_repack` (and `.dylib` files) with your Developer ID; users may need to allow the binary under **Privacy & Security**. Prefer non–Mac App Store Lightroom for `LrTasks.execute`. |
| **Wrong colors / HDR**             | Ensure the photo is an HDR edit; keep **Image Sizing** consistent (the plug-in clones sizing for the HDR pass).                                                                                            |
| **Inspect Lightroom TIFF / SDR** | Enable **Save debug copies** in the filter: `_uhdr_sdr` and `_uhdr_hdr` files appear next to the export; see the session `uhdr_export_*.log` in that same folder.                                         |
| **HDR TIFF incomplete / “broken”** | The plug-in waits until the internal HDR file size stabilizes and the TIFF magic bytes look valid before copying or calling `uhdr_repack`. If exports still fail, reduce **Image Sizing** (large 32-bit HDR TIFFs are memory-heavy). |


---

## Public release checklist

- Run `./scripts/bundle_uhdr_for_plugin.sh` on an **arm64** Mac; smoke-test `ExportHDR.lrplugin/bin/uhdr_repack --inspect` on an output JPEG.
- **Codesign** (and optionally **notarize**) `bin/uhdr_repack` and bundled `*.dylib` files for smoother installs.
- Test the plug-in on a clean catalog: single photo and batch export.
- Confirm **HDR TIFF** auxiliary export produces a file (enable **Keep HDR TIFF temp folders** once for debugging).
- Zip `**ExportHDR.lrplugin`** as a bundle (include `bin/` contents).

---

## References

- [chemharuka/toGainMapHDR](https://github.com/chemharuka/toGainMapHDR) — Core Image HDR patterns.
- [fengshenx/LR_GainMap_HDR_Export_Plugin](https://github.com/fengshenx/LR_GainMap_HDR_Export_Plugin) — related Lightroom export hook (different toolchain).

