# Ultra HDR Export for Lightroom Classic

> **Warning:** Want **pro** HDR for Instagram / Threads, with real image-science support? Use Greg Benz’s **[Web Sharp Pro](https://gregbenzphotography.com/web-sharp-pro-panel)** Photoshop panel.
>
> This Lightroom plugin is purely vibecoded. I had no knowledge of images or HDR concepts — the goal was just to get Ultra HDR working on Instagram, and probably other web services.

> **Note:** Version 3 is only tested on macOS. Windows may have issues — please [open a GitHub issue](https://github.com/karachungen/lightroom-plugin-export-hdr/issues).

Lightroom Classic export destination that turns an SDR JPEG plus an internal HDR TIFF pass into one Ultra HDR gain-map JPEG (`.jpg`) via `uhdr_repack` (`google/libultrahdr`).

## What problem it solves

Lightroom can edit/export HDR, but HDR support across apps is still uneven. Instagram/Threads HDR uploads are format-sensitive, so valid HDR files may not always display or upload consistently. This plugin gives a direct Lightroom Classic workflow for one Ultra HDR gain-map JPEG output with an SDR fallback path.

## What the plugin does

- Adds **Export To → ULTRA HDR**.
- Renders JPEG as the SDR base with your chosen sizing, **forced to Display P3** so the Ultra HDR primary is not clipped to sRGB first. Leave **Image Sizing** off to keep the photo’s aspect and pixel size; Instagram crops in Ultra HDR are optional.
- **Opens Ultra HDR** (`uhdr_repack --edit`) as soon as SDR JPEGs are ready.
- Renders each Lightroom HDR TIFF on demand (Gain/HDR preview, or one photo at a time on **Encode**). The TIFF short edge is capped at **2880px** (never upscaled); the filmstrip notes when that happened for performance. **Encode** deletes the TIFF after that photo.
- Replaces each exported base path with the final Ultra HDR `.jpg` for photos included in **Encode N photos**.

## How it works

1. Lightroom renders the normal export (SDR base) using your export settings.
2. The plugin writes a session JSON and launches bundled **Ultra HDR** (web UI + native HDR viewport).
3. The editor asks Lightroom for the current photo’s HDR TIFF when you open **Gain** or **HDR** (SDR review stays live). **Encode N photos** renders and encodes one TIFF at a time, then deletes it.
4. **Encode N photos** writes Ultra HDR JPEGs and the plugin promotes them to your export folder.

## Local development

From the repo root, build and refresh the in-tree plug-in (no zip):

```bash
./scripts/build_plugin.sh
# same as: ./scripts/build_plugin.sh install
```

The default `./scripts/build_plugin.sh` ensures dependencies, then runs build, bundle, and test. It updates `ExportHDR.lrplugin/bin/` in place. In Lightroom: **File → Plug-in Manager → Add** (or Reload) and select the `ExportHDR.lrplugin` folder in this repo.

Do **not** unzip release archives over the dev `ExportHDR.lrplugin` folder — that can remove the Lua sources. Use `./scripts/build_plugin.sh all` only when you need a release zip (same as CI). `./scripts/build_plugin.sh all` is the local preflight and matches the macOS Actions job, including the zip.

## Install and use

1. Download plugin from [GitHub Releases](https://github.com/karachungen/lightroom-plugin-export-hdr/releases)
2. Install in Lightroom: `File -> Plug-in Manager -> Add` and select `ExportHDR.lrplugin`
3. In Export, choose **Export To → ULTRA HDR**. File Settings are JPEG, Display P3, for the SDR base.
4. Export — Ultra HDR opens on the SDR JPEGs. Open **Gain** or **HDR** to preview-encode the current photo (Lightroom then renders that HDR TIFF).
5. Click **Encode N photos** to write Ultra HDR JPEGs for the non-skipped queue.
