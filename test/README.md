# Encoder smoke test assets

Place sample inputs here for the smoke-test scripts:

- `hdr-raw.tif` — Lightroom-style HDR TIFF (Rec2020 HDR / extended range).
- `sdr.jpg` — SDR base JPEG at the **same pixel dimensions** as the TIFF.

Outputs (`out_uhdr.jpg`) are gitignored; the script writes them when run.

**Run:**
- macOS/Linux: `./scripts/run_uhdr_test.sh`
- Windows: `.\scripts\run_uhdr_test.ps1`

If these files are missing, the script exits with a clear message.
