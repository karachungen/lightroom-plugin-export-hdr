# Encoder smoke test assets

Place sample inputs here for the smoke-test scripts:

- `hdr-raw.tif` — Lightroom-style HDR TIFF (Rec2020 HDR / extended range).
- `sdr.jpg` — SDR base JPEG at the **same pixel dimensions** as the TIFF.

Outputs (`out_uhdr.jpg`) are gitignored; the script writes them when run.

**Run** (after `./scripts/build_plugin.sh bundle` or `all`):

- macOS / Git Bash: `./scripts/run_uhdr_test.sh` or `./scripts/build_plugin.sh test`
- Windows PowerShell: `.\scripts\build_plugin.ps1 test` (uses Git Bash when available)

The smoke scripts include a Cyrillic folder path test (`test/тест/`). Lightroom exports with Cyrillic filenames use ASCII staging inside the plug-in temp folder before calling `uhdr_repack`; the final JPEG is copied to the export path via `LrFileUtils` after encode.

**Windows cmd quoting:** `.\scripts\test_windows_cmd_quote.ps1` verifies the plug-in shell layer against a synthetic path containing space and `(1)` (requires built `uhdr_repack.exe` and test inputs).

If these files are missing, the script exits with a clear message.

**Unicode paths:** the smoke scripts also encode to `test/тест/out_uhdr.jpg` (Cyrillic folder name, normal output filename) to verify UTF-8 path handling on Windows and macOS. Inputs stay ASCII (`hdr-raw.tif`, `sdr.jpg`).
