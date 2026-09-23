# Encoder smoke test assets

Place sample inputs here for the smoke-test scripts:

- `hdr-raw.tif` — Lightroom-style HDR TIFF (Rec2020 HDR / extended range).
- `sdr.jpg` — SDR base JPEG at the **same pixel dimensions** as the TIFF.

Outputs (`out_uhdr.jpg`) are gitignored; the script writes them when run.

**Run** (after `./scripts/build_plugin.sh bundle` or `all`):

- macOS / Git Bash: `./scripts/run_uhdr_test.sh` or `./scripts/build_plugin.sh test`
- Windows PowerShell: `.\scripts\build_plugin.ps1 test` (uses Git Bash when available)

The smoke scripts include a Cyrillic folder path test (`test/тест/`). Lightroom exports with Cyrillic filenames use ASCII staging inside the plug-in temp folder before calling `uhdr_repack`; the final JPEG is copied to the export path via `LrFileUtils` after encode.

**Windows cmd quoting:** `.\scripts\test_windows_cmd_quote.ps1` verifies full-path shell invocation under a synthetic path containing space and `(N)`, and asserts the legacy `cd` + relative-exe pattern fails.

**macOS shell quoting:** `./scripts/test_macos_shell_quote.sh` stages the encoder under a path containing `Application Support`, asserts an unquoted binary path fails (issue #2), and asserts `CMD.shellQuote` + `Command.lua` `shellBinary` quoting succeed for encode/`--inspect`.

If these files are missing, the script exits with a clear message.

**HDR stop / color chart** (`test/hdr-chart/`): committed `hdr-chart.tif` (float linear Rec.2020, profile embedded) and `sdr-chart.jpg` (Display P3) at 1440×1920. Open the editor with `.\scripts\run_hdr_chart_edit.ps1` (Windows) or `./scripts/run_hdr_chart_edit.sh`. Rebuild with `uhdr_repack --write-hdr-chart test/hdr-chart`. Check encode recovery with `--check-hdr-chart` (writes gitignored `chart-uhdr.jpg`). R2020 +4 must come back as `(16, 0, 0)` and its gain map must stay chromatic.

**Unicode paths:** the smoke scripts also encode to `test/тест/out_uhdr.jpg` (Cyrillic folder name, normal output filename) to verify UTF-8 path handling on Windows and macOS. Inputs stay ASCII (`hdr-raw.tif`, `sdr.jpg`).
