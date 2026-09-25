# Encoder tests

The chart is `test/hdr-chart/hdr-chart.tif` and `test/hdr-chart/sdr-chart.jpg` at 1440×1920 (3:4). The TIFF is float32 little-endian, Deflate (compression 8), predictor 3, 16-row strips, linear Rec.2020. The JPEG is Display P3 with a 1024-entry sRGB EOTF table. The synthetic SDR clips each Display P3 channel.

Regenerate the committed chart, then check it against a fresh write:

```text
uhdr_repack --write-hdr-chart test/hdr-chart
uhdr_repack --check-hdr-chart-assets test/hdr-chart tools/uhdr_repack/build/test-out/hdr-chart
```

Sections:

- **A** `neutral` — stops −8 through +5 (14 patches). +5 is report-only.
- **B** `hue` — 12 hues at 30° on the Display P3 cube edge, at stops −4, −2, +0, +1, +2, +3, and +4.
- **C** `gamut` — R Y G C B M for sRGB, Display P3, and Rec.2020, at +0 and +3. Rec.2020 R, G, and B at +4 (`peak2020`) also require a chromatic gain map.
- **D** `sat` — 6 hues at 25/50/75/100% mixed with linear white, at +2.
- **E** `cc` — ColorChecker 24 at +0 and +2.
- **F** — a continuous neutral ramp from −6 to +5, and 24 hue bands at +0 and at +3.
- **G** — line pairs and a slanted edge at +0, +2, and +4. Visual only, no samples.

A 4, 6, and 8 px specular at +4 is report-only. Every gated sample lies inside the Instagram 4:5 crop (y 60–1860 of the 3:4 canvas). `--write-hdr-chart` fails if one does not. Narrower frames grade the samples they contain and count the rest as outside.

**Run** from the repo root, after the encoder is built:

```text
ctest --test-dir tools/uhdr_repack/build --output-on-failure
```

`./scripts/build_plugin.sh test` and `.\scripts\build_plugin.ps1 test` run the cache contract test, the Qt kit fixture test, `ctest`, and the platform quote regression. `build_plugin.ps1` forwards to `build_plugin.sh`. Tests write into `tools/uhdr_repack/build/test-out/`.

CTest registers 29 tests, plus `smoke.probe-sdr` and `smoke.preview-jpeg` when the GUI is built:

- `chart.write`, `chart.assets-current`, `chart.input.hdr`, `chart.input.sdr`
- `chart.color.original` and `chart.mono.original`
- `chart.<preset>.ig-<aspect>-1080` and `chart.<preset>.ig-<aspect>-native` for presets `color` and `mono` and aspects `1x1`, `4x5`, `3x4`, `191x100`
- `smoke.cli.default.encode`, `smoke.cli.default.verify`, `smoke.cli.slice-1x1.encode`, `smoke.cli.slice-1x1.verify`, `smoke.cli.feed-4x5.encode`, `smoke.cli.feed-4x5.verify`
- `smoke.utf8-path` — encodes the chart into `test-out/тест/out_uhdr.jpg`

**Windows cmd quoting:** `.\scripts\test_windows_cmd_quote.ps1` verifies full-path shell invocation under a synthetic path containing space and `(N)`, and asserts the legacy `cd` + relative-exe pattern fails.

**macOS shell quoting:** `./scripts/test_macos_shell_quote.sh` stages the encoder under a path containing `Application Support`, asserts an unquoted binary path fails (issue #2), and asserts `CMD.shellQuote` + `Command.lua` `shellBinary` quoting succeed for encode/`--inspect`.

Lightroom exports with Cyrillic filenames use ASCII staging inside the plug-in temp folder before calling `uhdr_repack`; the final JPEG is copied to the export path via `LrFileUtils` after encode.

**Lightroom end-to-end check:**

```text
1. Import test/hdr-chart/hdr-chart.tif into Lightroom. No edits, no crop, HDR on in Develop.
2. Export through the plug-in with "Keep HDR TIFF temp files" on. In the editor pick a preset
   (Color map or Mono map) and an Instagram frame, then Apply.
3. uhdr_repack --describe-input <kept HDR TIFF> --require-portable
     Must print reader=portable. Otherwise the real export uses an OS decoder; report the why.
4. uhdr_repack --check-hdr-chart-file <export.jpg> --manifest test/hdr-chart/chart-manifest.json --slice-aspect <frame>
     Leave out --slice-aspect for Original. The full-headroom pass must PASS.
5. uhdr_repack --verify-uhdr <export.jpg> --max-bytes 8388608
```

Open the same chart in the editor with `.\scripts\run_hdr_chart_edit.ps1` (Windows) or `./scripts/run_hdr_chart_edit.sh`.
