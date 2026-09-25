# HDR palette chart

## Goals

1. One Ultra HDR test that runs the same way on macOS and Windows and grades what a web browser and Instagram will show.
2. Cover every color and HDR stop the plug-in can deliver, in every Instagram frame, at full HDR headroom, at partial headroom, and on SDR screens, so no color is lost on the way.
3. The test encodes through the same code the editor's Apply button uses, with the same presets and the same input decoders. A passing test means a real export of the same chart passes.

## Production parity

The test and a real export share one path:

```text
Lightroom HDR TIFF (ZIP, float, linear Rec.2020)  ─┐
Lightroom SDR JPEG (Display P3, sRGB curve)        ─┤→ encode_session_item → encode_from_paths → libultrahdr
Editor preset (Color map / Mono map), crop, size   ─┘
```

- **Input readers are portable.** `load_hdr_tiff_raw` and `load_sdr_base_raw` first call portable readers with no OS code: a float TIFF reader (uncompressed or Deflate, predictor 1 or 3) and a libjpeg SDR loader. WIC and Core Image stay only as a fallback for inputs the portable readers reject (PNG bases, CMYK JPEGs, unknown profiles), and the fallback prints one line saying why. `--describe-input <file> --require-portable` fails when a file would hit the fallback.
- **The chart looks like a Lightroom export.** `hdr-chart.tif` is written the way Lightroom writes its ZIP HDR TIFF (Deflate, floating-point predictor, linear Rec.2020 profile), so the test exercises the decoder a real export uses.
- **Presets live in one place.** `encode_presets.h` defines Color map and Mono map. The editor UI receives them from C++, the CLI takes `--preset`, the GUI self-test and the chart test use the same table.
- **The chart test calls the Apply function.** `--check-hdr-chart` builds a one-item `PreviewSession` and calls `encode_session_item`, the function `main_window.cpp` calls on Apply. Crop, output size, and preset go through the same fields the editor sets.

Out of scope, with the reason:

- Gain-map editing (`--gainmap-in`, `compute_auto_gainmap`) still decodes the SDR base through WIC or Core Image. It is a creative override, not the default color pipeline.
- Watermarks, PNG and TIFF SDR bases, and Lightroom's own HDR→SDR render are not graded.

## Files

The generator writes three files into the output directory:

- `hdr-chart.tif` — float32 RGB, little-endian, Deflate (compression 8) with floating-point predictor 3, 16-row strips, linear Rec.2020, 1.0 = SDR white.
- `sdr-chart.jpg` — Display P3, sRGB transfer, same pixels after the SDR rule below.
- `chart-manifest.json` — version 2. Every measured point.

Canvas is 1440×1920 (3:4). Flat patches are large enough that a center sample survives a 1080 px export and a gain map at 1/2 resolution.

## Layout

| Section | What | Gate |
| --- | --- | --- |
| A `neutral` | Stops −8 through +5 (14 patches, 88×48, rows y 62–110) | Pass, except +5 is report-only |
| B `hue` | 12 hues at 30° on the Display P3 cube edge, stops −4, −2, +0, +1, +2, +3, +4 (56 px patches) | Pass |
| C `gamut` | R Y G C B M for sRGB, Display P3, and Rec.2020, at +0 and +3 | Pass |
| `peak2020` | Rec.2020 R, G, B at +4 | Pass, plus a chromatic gain-map check |
| D `sat` | 6 hues × 25/50/75/100% mixed with linear white, at +2 | Pass |
| E `cc` | ColorChecker 24 (BabelColor sRGB, converted to linear Rec.2020) at +0 and +2 | Pass |
| F `grad-neutral` | Continuous neutral ramp from −6 to +5, one sample per stop | Pass, and monotonic |
| F `grad-hue0`, `grad-hue3` | 24 flat hue bands (every 15°) at +0 and at +3 | Pass |
| G | Line pairs and a slanted edge at +0, +2, +4 | Visual only, no samples |
| `specular` | 4, 6, and 8 px squares at +4 on a dark plate | Report only |

Every gated sample lies inside the Instagram 4:5 crop (y 60–1860 of the 3:4 canvas). `--write-hdr-chart` fails if one does not. Narrower frames (1:1, 1.91:1) grade the samples they contain and count the rest as outside.

Hues on the P3 boundary are the linear Display P3 RGB cube edge (one channel 0, one channel 1), converted to Rec.2020 and scaled so the peak channel is 1, then multiplied by 2^stop. The −4 and −2 hue stops cover dark saturated colors, where 8-bit bases and 4:2:0 chroma lose color first.

ColorChecker values are the published BabelColor / X-Rite 8-bit sRGB set. They are decoded with the sRGB EOTF, converted to linear Rec.2020, then scaled by 2^stop.

The neutral ladder and the neutral ramp are monotonic series. A gated sample may not fall more than 0.20 stop below the previous gated sample.

## SDR rule

The synthetic SDR JPEG clips each Display P3 channel to 0–1 after converting from linear Rec.2020, then encodes with the sRGB OETF. A hue-preserving scale (divide by the Rec.2020 peak) is not used: it gives every positive channel the same gain, so a Rec.2020 primary's small extra P3 channel fails the chromatic check.

## ICC

`icc_profile` builds matrix/TRC ICC v2.1 profiles and classifies any embedded profile.

- Header: version `0x02100000`, class `mntr`, space `RGB `, PCS `XYZ `, signature `acsp` at byte 36, PCS illuminant D50.
- `desc` is `textDescriptionType`, `cprt` is `textType`, tags are sorted, the three TRC tags share one curve.
- The TIFF profile uses `curv` count 0 (linear). The JPEG profile uses a 1024-entry `curv` table of the **sRGB EOTF** (device value → linear light). The current builder writes the OETF, which inverts the curve: browsers and Core Image would decode the SDR base too dark in the midtones. The fix changes the builder and the validator.

`classify_icc(bytes)` recognizes a profile by what it does, not by its name:

- Primaries: the `rXYZ`/`gXYZ`/`bXYZ` colorants match the Bradford D65→D50 adaptation of Rec.2020, Display P3, or sRGB within 0.005.
- Transfer: all three TRCs are linear (`curv` count 0, gamma 1.0, or a table/`para` curve within 0.002 of identity) or the sRGB EOTF (table or `para` types 0–4 within 0.002 at 17 points).
- A profile with an `A2B0` LUT, or anything else, is unknown.

`read_embedded_icc(path)` returns the TIFF tag 34675 or the reassembled `ICC_PROFILE` APP2 chunks of a JPEG's primary image. No OS color manager is used anywhere in the checks. `icc_os_win.cpp` and `icc_os_mac.mm` are deleted.

## Portable input readers

The float TIFF reader accepts:

- byte order II or MM; strips, not tiles; `PlanarConfiguration` 1; `Orientation` 1;
- 3 or 4 samples of 32-bit IEEE float (a fourth sample is ignored);
- compression 1, 8, or 32946 (Deflate through miniz), predictor 1 or 3;
- no profile (read as linear Rec.2020), or a linear profile with Rec.2020, Display P3, or sRGB primaries, converted to Rec.2020.

The SDR JPEG loader accepts YCbCr, grayscale, or RGB JPEGs that are untagged (read as sRGB) or tagged Display P3 or sRGB primaries with the sRGB curve. It decodes with libjpeg, scales to the master size, crops, and converts sRGB to Display P3. Both readers report `supported`/`why` through `describe_float_tiff` and `describe_sdr_jpeg`.

## Presets

| Preset | JPEG quality | Gain map | Boost | Target peak |
| --- | --- | --- | --- | --- |
| `color` (Color map) | 95 / 95 | RGB, scale 1 | 1–16 | 3250 nits |
| `mono` (Mono map) | 95 / 95 | luma, scale 2 | 1–4.92 | 1000 nits |

`color_map_preset()` is `EncodeOptions{}`, so the session default and the Color map card are the same value by construction.

## Manifest

```json
{
  "version": 2,
  "width": 1440,
  "height": 1920,
  "max_content_boost": 16.0,
  "samples": [
    {
      "id": "neutral/+0",
      "group": "neutral",
      "series": "neutral",
      "series_index": 8,
      "monotonic": true,
      "exposure_ref": true,
      "rect": [0, 0, 40, 40],
      "expected_rec2020": [1.0, 1.0, 1.0],
      "gate": {"encoder": "pass", "lightroom": "pass"},
      "chromatic": ""
    }
  ]
}
```

`rect` is the sample window in chart pixels, not the whole patch. `chromatic` is `R`, `G`, or `B` for the three +4 Rec.2020 primaries, otherwise empty. `exposure_ref` is set on neutral +0, +1, and +2.

## Checks

### Delivery check

`--verify-uhdr <jpg>` fails unless the file is what browsers and Instagram need:

- Ultra HDR with an MPF index, the primary `hdrgm` XMP, and the ISO 21496-1 APP2 block;
- a primary ICC that classifies as Display P3 with the sRGB curve (otherwise browsers show the base as sRGB and saturated colors shift);
- a gain map of image size / `--gainmap-scale` (floor or ceiling);
- the xmpRights URLs;
- optional `--expect-size WxH`, `--max-bytes N` (Instagram's 8 MB), and `--expect-single <aspect>` (no numbered slices).

### Encoder mode

`uhdr_repack --check-hdr-chart <dir> [--out <jpg>] [--preset color|mono] [--slice-aspect A] [--crop-offset F] [--out-width N] [other encode options]` parses the options with the same function the CLI encoder uses, then calls `encode_session_item`. It rejects `--gainmap-in`, `--watermark-config`, and a slice count other than 1.

After encoding it:

1. Runs the delivery check with the size from `resolve_item_export_size`, the preset's gain-map scale, and the 8 MB limit.
2. Finds the chart frame with `compute_slices` (the same planner the encoder uses) and maps sample windows into the output.
3. Reads the gain-map metadata from the file (content boost, offsets, HDR capacity, base or alternate color space, RGB or luma map).
4. Decodes and grades three passes, like a browser on three kinds of screen:
   - `boost=1` (SDR screen): every sample must match the SDR rule; this is the fallback Instagram and browsers show without HDR.
   - `boost=4` (partial headroom), when the capacity is above 4: in-P3 samples must match the ISO 21496-1 blend `(sdr + k_sdr)·G^w − k_hdr`, with `G` the clamped content gain and `w` the headroom weight. Out-of-P3 samples are report-only, because the blend space decides how they clip.
   - full capacity: an RGB map must return the manifest value; a luma map is graded with the same blend model on in-P3 samples.
5. Checks the chromatic gain map of the `peak2020` patches in the full-capacity pass of an RGB map.

### Lightroom mode

`uhdr_repack --check-hdr-chart-file <jpg-or-tif> [--manifest <path>] [--slice-aspect A] [--crop-offset F]` does not encode. A JPEG gets the same three passes, but only the full-capacity pass gates, because Lightroom renders its own SDR base. A TIFF is loaded through `load_hdr_tiff_raw` and graded once. The aspect must match the frame within 2%.

### Tolerances

- Encoder: 0.15 stop on the peak channel, 0.05 on hue (max channel error after normalizing each color by its peak).
- Lightroom: 0.25 stop and 0.08 hue.
- Hue is not graded when the expected peak is below 0.01.
- Chromatic gain map: named channel at least 0.50, each other channel at most 0.10, gap at least 0.50.

Report-only points are printed and do not fail the run. Every pass ends with one summary line; the exit code is the gate:

```text
HDR_CHART encoder boost=16.00 PASS gated=N failed=0 outside=M
```

One set of tolerances applies on macOS and Windows. If the platforms genuinely differ, the tolerance is widened for both and the reason is written on the constant.

## Cross-platform testing

The only per-OS step is opening a terminal where CMake and the compiler are available (the MSVC developer environment on Windows).

```text
cmake --build tools/uhdr_repack/build
ctest --test-dir tools/uhdr_repack/build --output-on-failure
```

Rules:

- Test logic lives in `uhdr_repack` subcommands; the exit code is the verdict. No test needs bash, PowerShell, or Python, or depends on a log regex.
- Test code has no `#ifdef _WIN32` or `#ifdef __APPLE__`. Every input a test encodes goes through the portable readers.
- Tests read generated inputs from `<build>/test-out/hdr-chart` and write into `<build>/test-out/`, never into `test/`.

### CTest entries

| Test | Command | Notes |
| --- | --- | --- |
| `chart.write` | `--write-hdr-chart test-out/hdr-chart` | Fixture setup `chart` |
| `chart.assets-current` | `--check-hdr-chart-assets test/hdr-chart test-out/hdr-chart` | TIFF and manifest byte for byte |
| `chart.input.hdr`, `chart.input.sdr` | `--describe-input <file> --require-portable` | Chart inputs never hit an OS decoder |
| `chart.<preset>.original` | `--check-hdr-chart … --preset <preset>` | Full frame, SDR JPEG passed through |
| `chart.<preset>.ig-<aspect>-1080` | `… --slice-aspect <aspect> --out-width 1080` | Instagram 1× feed size |
| `chart.<preset>.ig-<aspect>-native` | `… --slice-aspect <aspect>` | Native crop, up to 2× |
| `smoke.cli.*` | plug-in CLI encode of the chart + `--verify-uhdr` | Default, 1:1 single, 4:5 feed 1080 |
| `smoke.utf8-path` | `--check-utf8-path <chart tif> <chart jpg> test-out` | Cyrillic folder |
| `smoke.probe-sdr`, `smoke.preview-jpeg` | chart SDR | GUI builds only |

Presets are `color` and `mono`; aspects are `1x1`, `4x5`, `3x4`, `191x100`. Labels are `chart` and `smoke`.

`--check-hdr-chart-assets` compares `hdr-chart.tif` and `chart-manifest.json` byte for byte. miniz and the writer are deterministic, so a difference means the committed assets are stale. The SDR JPEG is not compared, because libjpeg output may differ across builds.

### Scripts and CI

- Removed: `scripts/run_uhdr_test.sh`, `scripts/run_uhdr_test.ps1`, `scripts/check_hdr_chart_snapshot.sh`, `scripts/check_hdr_chart_snapshot.ps1`.
- `build_plugin.sh test`, `build_plugin.ps1 test`, and `test_uhdr_preview_suite.sh` call the `ctest` line above.
- The release workflow runs that `ctest` line in one unconditional step on both runners.
- `test/README.md` documents the Lightroom end-to-end check: import `hdr-chart.tif` with no edits, export through the plug-in with any preset and Instagram frame, keep the HDR TIFF, then run `--describe-input <kept tif> --require-portable` and `--check-hdr-chart-file <export.jpg> --slice-aspect <frame>`.
