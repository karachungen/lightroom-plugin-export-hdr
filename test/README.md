# Encoder smoke test assets

Place sample inputs here for `scripts/run_uhdr_test.sh`:

- `hdr-raw.tif` — Lightroom-style HDR TIFF (Rec2020 HDR / extended range).
- `sdr.jpg` — SDR base JPEG at the **same pixel dimensions** as the TIFF.

Outputs (`out_uhdr.jpg`) are gitignored; the script writes them when run.

If these files are missing, the script exits with a clear message.
