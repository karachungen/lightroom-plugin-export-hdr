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
- Renders JPEG as the SDR base with your chosen sizing. Leave **Image Sizing** off to keep the photo’s aspect and pixel size; Instagram crops in Ultra HDR are optional.
- **Opens Ultra HDR** (`uhdr_repack --edit`) as soon as SDR JPEGs are ready.
- Starts Lightroom HDR TIFF renders for the whole queue when the editor opens (the selected photo is first). **Encode** waits for any leftover TIFFs.
- Replaces each exported base path with the final Ultra HDR `.jpg` for photos included in **Encode N photos**.

## How it works

1. Lightroom renders the normal export (SDR base) using your export settings.
2. The plugin writes a session JSON and launches bundled **Ultra HDR** (web UI + native HDR viewport).
3. The editor asks Lightroom for every photo’s HDR TIFF as soon as it opens (SDR review stays live). Opening **Gain** or **HDR** preview-encodes the current photo, jumping that TIFF to the front of the Lightroom queue if it is still pending.
4. **Encode N photos** waits for any leftover HDR TIFFs, encodes Ultra HDR JPEGs, and the plugin promotes them to your export folder.

## Local development

From the repo root, build and refresh the in-tree plug-in (no zip):

```bash
./scripts/build_plugin.sh
# same as: ./scripts/build_plugin.sh install
```

This runs **build → bundle → test** and updates `ExportHDR.lrplugin/bin/` in place. In Lightroom: **File → Plug-in Manager → Add** (or Reload) and select the `ExportHDR.lrplugin` folder in this repo.

Do **not** unzip release archives over the dev `ExportHDR.lrplugin` folder — that can remove the Lua sources. Use `./scripts/build_plugin.sh all` only when you need a release zip (same as CI).

## Install and use

1. Download plugin from [GitHub Releases](https://github.com/karachungen/lightroom-plugin-export-hdr/releases)
2. Install in Lightroom: `File -> Plug-in Manager -> Add` and select `ExportHDR.lrplugin`
3. In Export, choose **Export To → ULTRA HDR**. File Settings are JPEG for the SDR base.
4. Export — Ultra HDR opens on the SDR JPEGs and starts Lightroom HDR TIFF renders in the background. Open **Gain** or **HDR** to preview-encode the current photo.
5. Click **Encode N photos** to write Ultra HDR JPEGs for the non-skipped queue.
