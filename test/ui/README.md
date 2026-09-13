# UI preview fixtures

Runtime paths in `preview_session.json` are **relative to the repository root** (current working directory when running the demo).

## Fixtures (not in git)

Large JPEG/TIFF pairs are **not in git**. Put them in `test/ui/fixtures/` yourself, or copy at **build time** via `scripts/copy_ui_fixtures.sh` (invoked from `build_plugin.sh bundle`) by setting `UHDR_FIXTURES_SRC` to a folder that contains the required pairs.

```bash
UHDR_FIXTURES_SRC=/path/to/export/test ./scripts/copy_ui_fixtures.sh
```

If `UHDR_FIXTURES_SRC` is unset and the files already exist under `test/ui/fixtures/`, the copy script leaves them in place.

## Demo

```bash
./scripts/build_plugin.sh bundle   # copies fixtures + builds encoder
./scripts/run_uhdr_preview_demo.sh # Ultra HDR (web UI + HDR viewport)
```

Interactive `--edit` opens the embedded web UI (`tools/uhdr_repack/web/`) in WKWebView (macOS) or WebView2 (Windows). Encode settings, gain-map edit, and slice preview live there; Live/Final HDR uses the native QRhi panel.

## Automated test suite

Covers headless encode, gain map compute/edit/encode, session JSON, Qt offscreen self-test (brush/eraser/smooth/heatmap/HDR composite/apply batch):

```bash
CMAKE_PREFIX_PATH=/opt/homebrew/opt/qt ./scripts/build_plugin.sh build
./scripts/test_uhdr_preview_suite.sh
```

Optional UI screenshot (requires macOS display):

```bash
./scripts/capture_preview_screenshot.sh
# -> test/ui/preview_out/preview_screenshot.png
```

Requires an **HDR-capable display** for interactive `--edit`; `--self-test` runs offscreen without a monitor.
