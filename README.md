# Ultra HDR Export for Lightroom Classic

Lightroom Classic export filter that turns your normal export plus an internal HDR TIFF pass into one Ultra HDR gain-map JPEG (`.jpg`) via `uhdr_repack` (`google/libultrahdr`).

## What problem it solves

Lightroom can edit/export HDR, but HDR support across apps is still uneven. Instagram/Threads HDR uploads are format-sensitive, so valid HDR files may not always display or upload consistently. This plugin gives a direct Lightroom Classic workflow for one Ultra HDR gain-map JPEG output with an SDR fallback path.

## What the plugin does

- Adds export filter `Ultra HDR Export -> Encode Ultra HDR JPEG (uhdr_repack)`.
- Keeps your normal export as SDR base (usually JPEG) with your chosen sizing.
- Runs a second internal export as temporary `32-bit` `Rec2020_hdr` TIFF.
- Merges HDR TIFF + SDR base into one Ultra HDR `.jpg` at the same output path.
- Optional slicing (`1:1` or `4:5`) writes numbered full-height Ultra HDR slices next to the main file.

## How it works

1. Lightroom renders the normal export (SDR base) using your export settings.
2. The plugin runs an internal HDR TIFF export with matching dimensions.
3. Bundled `uhdr_repack` encodes the gain-map JPEG and replaces the base file path with the final Ultra HDR `.jpg`.

## Install and use

1. Download plugin from [GitHub Releases](https://github.com/karachungen/lightroom-plugin-export-hdr/releases)
2. Install in Lightroom: `File -> Plug-in Manager -> Add` and select `ExportHDR.lrplugin`
3. In Export, keep `File Settings` as `JPEG` for the main pass.
4. In `Post-Process Actions`, add `Ultra HDR Export -> Encode Ultra HDR JPEG (uhdr_repack)`.
5. Export and review `uhdr_export_<timestamp>.log` next to the export folder when troubleshooting.
