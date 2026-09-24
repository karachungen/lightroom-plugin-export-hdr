# HDR palette chart Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An OS-independent Ultra HDR test that grades every color and HDR stop of a 1440×1920 palette chart in every Instagram frame, at SDR, partial, and full headroom, through the exact code path the editor's Apply button uses.

**Architecture:** The chart generator, manifest, and grader live in `tools/uhdr_repack/src/hdr_chart.cpp`. This plan makes the production input readers portable (float TIFF with Deflate via miniz, libjpeg SDR loader), fixes the Display P3 ICC curve, moves the presets into one C++ table, and makes `--check-hdr-chart` call `encode_session_item` with the same options the editor sends. Every verdict is a `uhdr_repack` exit code registered with CTest.

**Tech Stack:** C++17, CMake 3.23, CTest, libultrahdr v2.0.2, libjpeg-turbo (vendored by libultrahdr), miniz 3.0.2 (new, single-file release), nlohmann_json.

**Spec:** `docs/superpowers/specs/2026-09-24-hdr-palette-chart-design.md`

## Review findings this plan fixes

The previous plan tested a different path from the one users run:

- **Inverted ICC curve.** `curv_srgb()` in `icc_profile.cpp` writes the sRGB OETF. ICC TRCs map device values to linear light, so the table must be the EOTF. Core Image and browsers honor the profile and decode `sdr-chart.jpg` wrong; WIC's path ignores it. The validator checks the same wrong curve, so it passes.
- **OS decoders on the real path.** Lightroom exports a ZIP-compressed HDR TIFF (`UHDRSettings.lua`). `load_identity_rec2020_tiff` accepts only uncompressed TIFFs, so every real export goes through WIC on Windows and Core Image on macOS, and Core Image also resizes differently. The chart TIFF is uncompressed, so the test never ran that path. SDR crops use WIC (nearest neighbor, profile detected by name) or Core Image (smooth, color-managed).
- **Different encode call.** The chart check calls `encode_from_paths` with hard-coded options. The editor calls `encode_session_item` with presets hard-coded again in `web/app.js` and `gui_self_test.cpp`.
- **Frames never graded.** The chart check only encodes the full 3:4 frame and fails any crop. The neutral row sits at y 22–110, so Instagram's 4:5 crop (y 60–1860) cuts it.
- **SDR and partial headroom never graded.** Only a decode at boost 16 was checked, and the base primary ICC (what browsers use) was never checked.
- **Missing dark saturated colors.** Hue patches start at stop +0.
- **Missing fixtures.** Smoke tests needed `test/hdr-raw.tif` and `test/sdr.jpg`, which are not in the repo. They now use the generated chart.

## Global Constraints

- Canvas 1440×1920 (3:4). `hdr-chart.tif`: float32 little-endian, Deflate (compression 8), predictor 3, 16-row strips, linear Rec.2020 ICC v2.1. `sdr-chart.jpg`: Display P3 ICC with a 1024-entry sRGB **EOTF** table.
- Encoder tolerances 0.15 stop / 0.05 hue; Lightroom 0.25 stop / 0.08 hue; hue skipped below peak 0.01. Chromatic gain map: named channel ≥ 0.50, others ≤ 0.10, gap ≥ 0.50. One tolerance set for both OSes.
- Presets come only from `encode_presets.h`. Color map = `EncodeOptions{}` (95/95, scale 1, boost 1–16, 3250 nits, RGB). Mono map = 95/95, scale 2, boost 1–4.92, 1000 nits, luma.
- Every input a test encodes goes through the portable readers. OS decoders remain only as a fallback that prints `... using the OS decoder`.
- Test code has no `#ifdef _WIN32` / `#ifdef __APPLE__`. No test needs bash, PowerShell, or Python, or gates on a log regex or JPEG bytes. Tests write into `<build>/test-out/`, never into `test/`.
- The Windows shell tool in this environment may not return exit codes. Run commands in a real terminal (MSVC developer shell on Windows) and read the exit code with `echo $LASTEXITCODE` (PowerShell) or `echo $?` (macOS).
- Paths below use `uhdr_repack` for `tools/uhdr_repack/build/uhdr_repack.exe` on Windows and `tools/uhdr_repack/build/uhdr_repack` on macOS, run from the repo root.

## File map

| File | Task | Change |
| --- | --- | --- |
| `include/icc_profile.h`, `src/icc_profile.cpp` | 1 | EOTF curve, `classify_icc`, `read_embedded_icc`; OS functions removed |
| `src/icc_os_win.cpp`, `src/icc_os_mac.mm` | 1 | Deleted |
| `CMakeLists.txt` | 1, 2, 4, 5, 7 | Sources, C language, miniz, tests |
| `include/tiff_float.h`, `src/tiff_float.cpp` | 2 | Portable float TIFF reader (Deflate, predictor, MM, profiles) |
| `src/tiff_input_win.cpp`, `src/tiff_input.mm` | 2 | Call the portable reader, log the fallback |
| `include/sdr_jpeg.h`, `src/sdr_jpeg.cpp` | 2 | New portable SDR JPEG loader |
| `src/sdr_input_win.cpp`, `src/sdr_input.mm`, `src/wic_utils.cpp` | 2 | Call the portable loader; WIC keeps PNG/TIFF bases |
| `src/cli_describe.cpp`, `include/cli.h` | 2, 4 | `--describe-input`, `parse_encode_flag` |
| `src/hdr_chart.cpp`, `include/hdr_chart.h` | 1, 2, 3, 6 | Writer, layout, assets check, Apply-path check, passes |
| `include/encode_presets.h`, `src/encode_presets.cpp` | 4 | New preset table |
| `src/cli_encode.cpp` | 2–6 | Shared flag parser, `--preset`, usage |
| `src/gui/gui_bridge.cpp`, `web/app.js`, `src/gui/gui_self_test.cpp` | 4 | Presets from C++ |
| `include/delivery_check.h`, `src/delivery_check.cpp` | 5 | `--verify-uhdr`, `--check-utf8-path`, `--encode-or-skip` |
| `include/gui/sdr_preview_image.h`, `src/gui/check_preview_jpeg.cpp` | 5 | `--check-preview-jpeg` |
| `src/main.cpp` | 2, 3, 5, 6 | Dispatch |
| `test/hdr-chart/*` | 3 | Regenerated and committed |
| scripts, CI, READMEs, CHANGELOG | 8 | One `ctest` command |

All source paths are under `tools/uhdr_repack/` unless they start with `test/`, `scripts/`, `docs/`, or `.github/`.

---

### Task 1: Classify embedded profiles and fix the Display P3 curve

**Files:**
- Modify: `tools/uhdr_repack/include/icc_profile.h`
- Modify: `tools/uhdr_repack/src/icc_profile.cpp`
- Modify: `tools/uhdr_repack/src/hdr_chart.cpp` (`write_hdr_chart_main` and one helper)
- Modify: `tools/uhdr_repack/CMakeLists.txt`
- Delete: `tools/uhdr_repack/src/icc_os_win.cpp`, `tools/uhdr_repack/src/icc_os_mac.mm`

**Interfaces:**
- Consumes: `bradford_d65_to_d50`, `read_be16`, `read_be32`, `read_s15`, `kRec2020ToXyzD65`, `kDisplayP3ToXyzD65` (all in `icc_profile.cpp`), `srgb_eotf` (`color_primaries.h`), `open_input_binary` (`path_io.h`).
- Produces:
  - `enum class IccPrimaries { kUnknown, kSrgb, kDisplayP3, kRec2020 };`
  - `enum class IccTransfer { kUnknown, kLinear, kSrgb };`
  - `struct IccClass { IccPrimaries primaries; IccTransfer transfer; };`
  - `IccClass classify_icc(const std::vector<uint8_t>& icc);`
  - `std::string icc_class_name(const IccClass& c);` e.g. `"Display P3 / sRGB curve"`.
  - `bool read_embedded_icc(const std::string& path, std::vector<uint8_t>* icc, std::string* error);`

- [ ] **Step 1: Replace the OS declarations and drop the OS sources**

In `tools/uhdr_repack/include/icc_profile.h`, delete `icc_accepted_by_os`, `image_has_color_profile`, and `read_tiff_float_rgb_os` with their comments. Change the `build_display_p3_icc` comment and add the new API after `validate_display_p3_icc`:

```cpp
/** ICC v2.1 matrix/TRC profile, 1024-entry sRGB EOTF table (device value -> linear), "Display P3". */
std::vector<uint8_t> build_display_p3_icc();
```

```cpp
enum class IccPrimaries { kUnknown, kSrgb, kDisplayP3, kRec2020 };
enum class IccTransfer { kUnknown, kLinear, kSrgb };

struct IccClass {
  IccPrimaries primaries = IccPrimaries::kUnknown;
  IccTransfer transfer = IccTransfer::kUnknown;
};

/**
 * Recognize an RGB matrix/TRC profile by its colorants and curves, not its name.
 * Profiles with an A2B0 LUT, or curves that are neither linear nor the sRGB EOTF, are kUnknown.
 */
IccClass classify_icc(const std::vector<uint8_t>& icc);

/** "Rec.2020 / linear", "Display P3 / sRGB curve", "unknown primaries / unknown curve", ... */
std::string icc_class_name(const IccClass& c);

/** ICC profile of a TIFF (tag 34675) or of a JPEG's primary image (ICC_PROFILE APP2 chunks). */
bool read_embedded_icc(const std::string& path, std::vector<uint8_t>* icc, std::string* error);
```

Delete `tools/uhdr_repack/src/icc_os_win.cpp` and `tools/uhdr_repack/src/icc_os_mac.mm`. In `tools/uhdr_repack/CMakeLists.txt`, remove `src/icc_os_mac.mm` from the `APPLE` list and `src/icc_os_win.cpp` from the `WIN32` list.

- [ ] **Step 2: Implement classification and profile extraction**

In `tools/uhdr_repack/src/icc_profile.cpp`, add `#include "path_io.h"`, `#include <fstream>`, and `#include <iterator>` to the includes. Inside the anonymous namespace, after `kDisplayP3ToXyzD65`, add:

```cpp
constexpr float kSrgbToXyzD65[9] = {0.41239080f, 0.35758434f, 0.18048079f, 0.21263901f, 0.71516868f,
                                    0.07219232f, 0.01933082f, 0.11919478f, 0.95053215f};
```

Inside the anonymous namespace, after `validate_profile`, add:

```cpp
struct TagRef {
  const uint8_t* p = nullptr;
  uint32_t n = 0;
};

TagRef find_tag(const std::vector<uint8_t>& icc, uint32_t sig) {
  if (icc.size() < 132) return {};
  const uint32_t ntags = read_be32(icc.data() + 128);
  if (ntags > (icc.size() - 132) / 12) return {};
  for (uint32_t i = 0; i < ntags; ++i) {
    const uint8_t* e = icc.data() + 132 + static_cast<size_t>(i) * 12u;
    if (read_be32(e) != sig) continue;
    const uint32_t off = read_be32(e + 4);
    const uint32_t n = read_be32(e + 8);
    if (off > icc.size() || n > icc.size() - off) return {};
    return {icc.data() + off, n};
  }
  return {};
}

bool colorant(const std::vector<uint8_t>& icc, uint32_t sig, float xyz[3]) {
  const TagRef t = find_tag(icc, sig);
  if (!t.p || t.n < 20 || std::memcmp(t.p, "XYZ ", 4) != 0) return false;
  for (int i = 0; i < 3; ++i) xyz[i] = read_s15(t.p + 8 + 4 * i);
  return true;
}

double para_eval(uint16_t type, const double* v, double x) {
  const double g = v[0], a = v[1], b = v[2], c = v[3], d = v[4], e = v[5], f = v[6];
  auto pw = [g](double base) { return base > 0.0 ? std::pow(base, g) : 0.0; };
  switch (type) {
    case 0: return pw(x);
    case 1: return (a != 0.0 && x >= -b / a) ? pw(a * x + b) : 0.0;
    case 2: return (a != 0.0 && x >= -b / a) ? pw(a * x + b) + c : c;
    case 3: return x >= d ? pw(a * x + b) : c * x;
    default: return x >= d ? pw(a * x + b) + e : c * x + f;
  }
}

template <typename Curve>
IccTransfer curve_verdict(Curve curve) {
  bool srgb = true;
  bool linear = true;
  for (int k = 0; k <= 16; ++k) {
    const double x = k / 16.0;
    const double y = curve(x);
    if (std::fabs(y - srgb_eotf(static_cast<float>(x))) > 0.002) srgb = false;
    if (std::fabs(y - x) > 0.002) linear = false;
  }
  if (srgb) return IccTransfer::kSrgb;
  if (linear) return IccTransfer::kLinear;
  return IccTransfer::kUnknown;
}

IccTransfer classify_trc(const TagRef& t) {
  if (!t.p || t.n < 12) return IccTransfer::kUnknown;
  if (std::memcmp(t.p, "curv", 4) == 0) {
    const uint32_t count = read_be32(t.p + 8);
    if (count > (t.n - 12) / 2) return IccTransfer::kUnknown;
    if (count == 0) return IccTransfer::kLinear;
    if (count == 1) {
      const double gamma = read_be16(t.p + 12) / 256.0;
      return curve_verdict([gamma](double x) { return std::pow(x, gamma); });
    }
    return curve_verdict([&](double x) {
      const double pos = x * (count - 1);
      const uint32_t i0 = std::min(static_cast<uint32_t>(pos), count - 1);
      const uint32_t i1 = std::min(i0 + 1, count - 1);
      const double y0 = read_be16(t.p + 12 + 2 * i0) / 65535.0;
      const double y1 = read_be16(t.p + 12 + 2 * i1) / 65535.0;
      return y0 + (y1 - y0) * (pos - i0);
    });
  }
  if (std::memcmp(t.p, "para", 4) == 0) {
    static const int kParams[5] = {1, 3, 4, 5, 7};
    const uint16_t type = read_be16(t.p + 8);
    if (type > 4 || t.n < 12u + 4u * static_cast<uint32_t>(kParams[type])) return IccTransfer::kUnknown;
    double v[7] = {1, 1, 0, 0, 0, 0, 0};
    for (int i = 0; i < kParams[type]; ++i) v[i] = read_s15(t.p + 12 + 4 * i);
    return curve_verdict([type, &v](double x) { return para_eval(type, v, x); });
  }
  return IccTransfer::kUnknown;
}

bool fail_read(std::string* error, const std::string& msg) {
  if (error) *error = msg;
  return false;
}

uint16_t tiff16(const uint8_t* p, bool le) {
  return le ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : read_be16(p);
}

uint32_t tiff32(const uint8_t* p, bool le) {
  return le ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)
            : read_be32(p);
}

bool icc_from_tiff(const std::vector<uint8_t>& file, std::vector<uint8_t>* icc, std::string* error) {
  const bool le = file[0] == 'I';
  const uint32_t ifd = tiff32(file.data() + 4, le);
  if (static_cast<size_t>(ifd) + 2 > file.size()) return fail_read(error, "TIFF IFD is past the end of the file");
  const uint16_t ntags = tiff16(file.data() + ifd, le);
  for (uint16_t i = 0; i < ntags; ++i) {
    const size_t at = ifd + 2u + static_cast<size_t>(i) * 12u;
    if (at + 12 > file.size()) return fail_read(error, "TIFF IFD entry is past the end of the file");
    const uint8_t* e = file.data() + at;
    if (tiff16(e, le) != 34675) continue;
    const uint32_t count = tiff32(e + 4, le);
    if (count < 128) return fail_read(error, "TIFF ICC tag is shorter than an ICC header");
    const uint32_t off = tiff32(e + 8, le);
    if (static_cast<size_t>(off) + count > file.size()) return fail_read(error, "TIFF ICC tag is past the end of the file");
    icc->assign(file.begin() + off, file.begin() + off + count);
    return true;
  }
  return fail_read(error, "TIFF has no ICC profile (tag 34675)");
}

bool icc_from_jpeg(const std::vector<uint8_t>& file, std::vector<uint8_t>* icc, std::string* error) {
  struct Chunk {
    int seq = 0;
    std::vector<uint8_t> data;
  };
  std::vector<Chunk> chunks;
  int total = -1;
  size_t i = 2;
  while (i + 1 < file.size()) {
    if (file[i] != 0xff) return fail_read(error, "JPEG marker was not FF");
    while (i < file.size() && file[i] == 0xff) ++i;
    if (i >= file.size()) break;
    const uint8_t marker = file[i++];
    if (marker == 0xd9 || marker == 0xda) break;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (i + 2 > file.size()) return fail_read(error, "JPEG marker length is truncated");
    const uint16_t seglen = read_be16(file.data() + i);
    if (seglen < 2 || i + seglen > file.size()) return fail_read(error, "JPEG marker is past the end of the file");
    if (marker == 0xe2 && seglen >= 16 && std::memcmp(file.data() + i + 2, "ICC_PROFILE", 12) == 0) {
      const uint8_t* p = file.data() + i + 2;
      const int seq = p[12];
      const int count = p[13];
      if (seq < 1 || count < 1 || seq > count) return fail_read(error, "JPEG ICC chunk sequence is invalid");
      if (total < 0) total = count;
      if (count != total) return fail_read(error, "JPEG ICC chunks disagree on the chunk count");
      chunks.push_back(Chunk{seq, std::vector<uint8_t>(p + 14, file.data() + i + seglen)});
    }
    i += seglen;
  }
  if (chunks.empty() || static_cast<int>(chunks.size()) != total) {
    return fail_read(error, "JPEG has no complete ICC_PROFILE sequence");
  }
  std::sort(chunks.begin(), chunks.end(), [](const Chunk& a, const Chunk& b) { return a.seq < b.seq; });
  icc->clear();
  for (int seq = 1; seq <= total; ++seq) {
    const Chunk& c = chunks[static_cast<size_t>(seq - 1)];
    if (c.seq != seq) return fail_read(error, "JPEG ICC chunk sequence has a gap");
    icc->insert(icc->end(), c.data.begin(), c.data.end());
  }
  if (icc->size() < 128) return fail_read(error, "JPEG ICC profile is shorter than an ICC header");
  return true;
}
```

After the anonymous namespace closes, next to the other public functions, add:

```cpp
IccClass classify_icc(const std::vector<uint8_t>& icc) {
  IccClass out;
  if (icc.size() < 132 || std::memcmp(icc.data() + 16, "RGB ", 4) != 0 ||
      std::memcmp(icc.data() + 20, "XYZ ", 4) != 0 || find_tag(icc, 0x41324230u).p) {
    return out;
  }
  float r[3];
  float g[3];
  float b[3];
  if (colorant(icc, 0x7258595Au, r) && colorant(icc, 0x6758595Au, g) && colorant(icc, 0x6258595Au, b)) {
    const struct {
      IccPrimaries id;
      const float* m;
    } cands[] = {{IccPrimaries::kRec2020, kRec2020ToXyzD65},
                 {IccPrimaries::kDisplayP3, kDisplayP3ToXyzD65},
                 {IccPrimaries::kSrgb, kSrgbToXyzD65}};
    const float* got[3] = {r, g, b};
    for (const auto& cand : cands) {
      float m[9];
      bradford_d65_to_d50(cand.m, m);
      float worst = 0.0f;
      for (int ch = 0; ch < 3; ++ch) {
        for (int row = 0; row < 3; ++row) worst = std::max(worst, std::fabs(got[ch][row] - m[row * 3 + ch]));
      }
      if (worst <= 0.005f) {
        out.primaries = cand.id;
        break;
      }
    }
  }
  const IccTransfer tr = classify_trc(find_tag(icc, 0x72545243u));
  if (tr == classify_trc(find_tag(icc, 0x67545243u)) && tr == classify_trc(find_tag(icc, 0x62545243u))) {
    out.transfer = tr;
  }
  return out;
}

std::string icc_class_name(const IccClass& c) {
  const char* p = c.primaries == IccPrimaries::kRec2020     ? "Rec.2020"
                  : c.primaries == IccPrimaries::kDisplayP3 ? "Display P3"
                  : c.primaries == IccPrimaries::kSrgb      ? "sRGB"
                                                            : "unknown primaries";
  const char* t = c.transfer == IccTransfer::kLinear ? "linear"
                  : c.transfer == IccTransfer::kSrgb ? "sRGB curve"
                                                     : "unknown curve";
  return std::string(p) + " / " + t;
}

bool read_embedded_icc(const std::string& path, std::vector<uint8_t>* icc, std::string* error) {
  if (!icc) return fail_read(error, "internal: null ICC output");
  std::ifstream in = open_input_binary(path);
  if (!in) return fail_read(error, "could not open " + path);
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (file.size() < 8) return fail_read(error, "file is too small to hold a color profile: " + path);
  if ((file[0] == 'I' && file[1] == 'I') || (file[0] == 'M' && file[1] == 'M')) return icc_from_tiff(file, icc, error);
  if (file[0] == 0xff && file[1] == 0xd8) return icc_from_jpeg(file, icc, error);
  return fail_read(error, "not a TIFF or JPEG: " + path);
}
```

- [ ] **Step 3: Point the chart writer at the embedded profiles**

In `tools/uhdr_repack/src/hdr_chart.cpp`, inside the anonymous namespace after `is_tiff_path`, add:

```cpp
bool embedded_profile_is(const std::string& path, IccPrimaries primaries, IccTransfer transfer,
                         std::string* error) {
  std::vector<uint8_t> icc;
  if (!read_embedded_icc(path, &icc, error)) return false;
  const IccClass got = classify_icc(icc);
  if (got.primaries != primaries || got.transfer != transfer) {
    if (error) {
      *error = path + " embeds " + icc_class_name(got) + ", expected " + icc_class_name({primaries, transfer});
    }
    return false;
  }
  return true;
}
```

In `write_hdr_chart_main`:

1. Delete the `icc_accepted_by_os` block (the `if` that calls it twice and its body).
2. Replace the `image_has_color_profile` block with:

```cpp
  if (!embedded_profile_is(tiff, IccPrimaries::kRec2020, IccTransfer::kLinear, &err) ||
      !embedded_profile_is(jpeg, IccPrimaries::kDisplayP3, IccTransfer::kSrgb, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
```

3. Replace the `read_tiff_float_rgb_os` block with the production reader:

```cpp
  std::vector<float> reread;
  int rw = 0;
  int rh = 0;
  if (!load_tiff_rec2020(tiff, &reread, &rw, &rh, &err) ||
      !samples_match(reread, rw, rh, samples, "encoder TIFF reader", &err)) {
    std::cerr << err << "\n";
    return 1;
  }
```

4. Change the last `std::cout` line to:

```cpp
  std::cout << kWidth << "x" << kHeight
            << " linear Rec.2020, embedded profiles Rec.2020 / linear and Display P3 / sRGB curve,"
               " readers match the manifest\n";
```

- [ ] **Step 4: Watch the new check catch the curve bug**

Reconfigure (the source list changed) with the preset `scripts/build_plugin.ps1` or `scripts/build_plugin.sh` used for this build directory, then:

```text
cmake --build tools/uhdr_repack/build --target uhdr_repack
uhdr_repack --write-hdr-chart tools/uhdr_repack/build/test-out/hdr-chart
```

Expected: exit 1 and `...sdr-chart.jpg embeds Display P3 / unknown curve, expected Display P3 / sRGB curve`. The table holds the OETF, which is neither linear nor the EOTF.

- [ ] **Step 5: Write and validate the EOTF**

In `tools/uhdr_repack/src/icc_profile.cpp`, in `curv_srgb()`, change `srgb_oetf(x)` to `srgb_eotf(x)`. In `validate_profile`, in the `expect_e` lambda, change `srgb_oetf(...)` to `srgb_eotf(...)`.

- [ ] **Step 6: Rebuild and write the chart**

```text
cmake --build tools/uhdr_repack/build --target uhdr_repack
uhdr_repack --write-hdr-chart tools/uhdr_repack/build/test-out/hdr-chart
```

Expected: exit 0; last line `1440x1920 linear Rec.2020, embedded profiles Rec.2020 / linear and Display P3 / sRGB curve, readers match the manifest`.

- [ ] **Step 7: Commit**

```text
git add tools/uhdr_repack/include/icc_profile.h tools/uhdr_repack/src/icc_profile.cpp tools/uhdr_repack/src/hdr_chart.cpp tools/uhdr_repack/CMakeLists.txt
git add -u tools/uhdr_repack/src/icc_os_win.cpp tools/uhdr_repack/src/icc_os_mac.mm
git commit -m "Classify ICC profiles from file bytes; write the sRGB EOTF in Display P3 profiles."
```

---

### Task 2: Portable production readers for Lightroom's TIFF and JPEG

**Files:**
- Modify: `tools/uhdr_repack/CMakeLists.txt`
- Modify: `tools/uhdr_repack/include/tiff_float.h`, `tools/uhdr_repack/src/tiff_float.cpp`
- Modify: `tools/uhdr_repack/src/tiff_input_win.cpp`, `tools/uhdr_repack/src/tiff_input.mm`
- Create: `tools/uhdr_repack/include/sdr_jpeg.h`, `tools/uhdr_repack/src/sdr_jpeg.cpp`
- Modify: `tools/uhdr_repack/src/sdr_input_win.cpp`, `tools/uhdr_repack/src/sdr_input.mm`, `tools/uhdr_repack/src/wic_utils.cpp`
- Create: `tools/uhdr_repack/src/cli_describe.cpp`
- Modify: `tools/uhdr_repack/include/cli.h`, `tools/uhdr_repack/src/main.cpp`, `tools/uhdr_repack/src/cli_encode.cpp` (`print_usage`)
- Modify: `tools/uhdr_repack/src/hdr_chart.cpp` (`reload_identity`)

**Interfaces:**
- Consumes: `classify_icc`, `icc_class_name`, `read_embedded_icc` (Task 1); `even_normalize`, `float_rgba_to_half_rgba` (`half_float.h`); `resize_hdr_lanczos`; `display_p3_to_rec2020`, `linear_srgb_to_rec2020`, `srgb_rgba8888_to_display_p3`; `rgba8888_to_yuv420_bt601` (`yuv_convert.h`); `load_binary_file` (`jpeg_container.h`); miniz `mz_uncompress`.
- Produces:
  - `struct FloatTiffInfo { bool is_tiff, supported; std::string why; unsigned width, height; uint16_t compression, predictor; bool big_endian, has_icc; IccClass icc; };`
  - `bool describe_float_tiff(const std::string&, FloatTiffInfo*, std::string*);`
  - `int load_float_tiff_portable(...)` and `int probe_float_tiff_portable(...)`, the same signatures and 1/0/−1 codes as the old `load_identity_rec2020_tiff` and `probe_identity_rec2020_tiff`, which are removed.
  - `void log_float_tiff_fallback(const std::string& path);`
  - `struct SdrJpegInfo { bool is_jpeg, supported; std::string why; unsigned width, height; bool has_icc; IccClass icc; };`
  - `bool describe_sdr_jpeg(const std::string&, SdrJpegInfo*, std::string*);`
  - `int load_sdr_jpeg_p3(const std::string& path, unsigned master_w, unsigned master_h, unsigned out_w, unsigned out_h, unsigned crop_x, unsigned crop_y, std::vector<uint8_t>* rgba, std::string* error);`
  - `bool rgba_p3_to_sdr_raw(const std::vector<uint8_t>& rgba, unsigned w, unsigned h, RawImageHolder* out, std::string* error);`
  - `void log_sdr_jpeg_fallback(const std::string& path);`
  - `decode_jpeg_rgba8` and `scale_crop_rgba8_buffer` move from `uhdr_repack::wic` to `uhdr_repack`, with unchanged signatures.
  - `int describe_input_main(int argc, char** argv);` for `--describe-input <path> [--require-portable] [--skip-if-missing]`. Exit codes: 0 described, 1 error or not portable with `--require-portable`, 77 missing with `--skip-if-missing`.

- [ ] **Step 1: Add miniz as a single-file source**

miniz's own CMake project would build a separate library with its own runtime flags, and the static-Qt Windows build forces `/MT` on `uhdr_repack`. The single-file release compiles into `uhdr_repack` with the same flags.

In `tools/uhdr_repack/CMakeLists.txt`, change both `project(...)` lines to add C:

```cmake
if(APPLE)
  project(uhdr_repack LANGUAGES C CXX OBJCXX)
else()
  project(uhdr_repack LANGUAGES C CXX)
endif()
```

After `FetchContent_MakeAvailable(nlohmann_json)`, add:

```cmake
FetchContent_Declare(
  miniz
  URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR no-cmake-project
)
FetchContent_MakeAvailable(miniz)
```

`SOURCE_SUBDIR` points at a directory that does not exist, so `FetchContent_MakeAvailable` downloads and extracts without calling `add_subdirectory`.

In `_UHDR_CORE_SOURCES`, add after `src/yuv_convert.cpp`:

```cmake
  src/sdr_jpeg.cpp
  src/cli_describe.cpp
  "${miniz_SOURCE_DIR}/miniz.c"
```

After `target_link_libraries(uhdr_repack PRIVATE nlohmann_json::nlohmann_json)`, add:

```cmake
target_include_directories(uhdr_repack PRIVATE "${miniz_SOURCE_DIR}")
target_compile_definitions(uhdr_repack PRIVATE
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO)
```

Reconfigure. Expected: `tools/uhdr_repack/build/_deps/miniz-src/miniz.c` and `miniz.h` exist. Find `miniz-3.0.2.zip` under `tools/uhdr_repack/build/_deps/`, run `cmake -E sha256sum <that zip>`, and add `URL_HASH SHA256=<printed hash>` under the `URL` line.

- [ ] **Step 2: Declare the portable TIFF reader**

Replace the body of `tools/uhdr_repack/include/tiff_float.h` with:

```cpp
#pragma once

#include "icc_profile.h"
#include "raw_image.h"
#include "slice_plan.h"

#include <cstdint>
#include <string>

namespace uhdr_repack {

/** What the portable TIFF reader saw. supported=false means an OS decoder reads the file; why says what was missing. */
struct FloatTiffInfo {
  bool is_tiff = false;
  bool supported = false;
  std::string why;
  unsigned width = 0;
  unsigned height = 0;
  uint16_t compression = 1;
  uint16_t predictor = 1;
  bool big_endian = false;
  bool has_icc = false;
  IccClass icc;
};

bool describe_float_tiff(const std::string& path, FloatTiffInfo* info, std::string* error);

/**
 * Read a strip TIFF of 32-bit IEEE float RGB(A), uncompressed or Deflate with predictor 1 or 3,
 * untagged or with a linear Rec.2020, Display P3, or sRGB-primaries profile, into linear
 * Rec.2020 half-float. Same code on macOS and Windows.
 * @return 1 loaded, 0 not this kind of TIFF (caller uses the OS decoder), -1 error.
 */
int load_float_tiff_portable(const std::string& path, RawImageHolder* out, std::string* error,
                             unsigned master_w = 0, unsigned master_h = 0,
                             const CropRect* crop = nullptr, unsigned dst_w = 0, unsigned dst_h = 0);

/** Even-normalized size of a TIFF the portable reader accepts. Same return codes as the loader. */
int probe_float_tiff_portable(const std::string& path, unsigned* master_w, unsigned* master_h,
                              std::string* error);

/** One stderr line saying why the portable reader passed on path. */
void log_float_tiff_fallback(const std::string& path);

}  // namespace uhdr_repack
```

- [ ] **Step 3: Implement the reader**

In `tools/uhdr_repack/src/tiff_float.cpp`, add `#include "color_primaries.h"` and `#include <miniz.h>` to the includes. Replace `struct RawFloatTiff`, `enum class FloatRead`, and the whole `read_identity_rec2020_float` function with:

```cpp
struct RawFloatTiff {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<float> rgb;
};

enum class FloatRead { kFallback, kRaw, kError };

FloatRead pass_on(FloatTiffInfo* info, const char* why) {
  info->supported = false;
  info->why = why;
  return FloatRead::kFallback;
}

uint32_t tiff_at(const uint8_t* p, uint16_t type, uint32_t i, bool le) {
  return type == 3 ? tiff_u16(p + i * 2u, le) : tiff_u32(p + i * 4u, le);
}

void undo_float_predictor(uint8_t* row, size_t values, uint32_t stride) {
  const size_t bytes = values * 4u;
  for (size_t i = stride; i < bytes; ++i) row[i] = static_cast<uint8_t>(row[i] + row[i - stride]);
  const std::vector<uint8_t> planes(row, row + bytes);
  for (size_t v = 0; v < values; ++v) {
    for (size_t b = 0; b < 4; ++b) row[v * 4u + b] = planes[(3 - b) * values + v];
  }
}

void swap_float_bytes(uint8_t* p, size_t values) {
  for (size_t v = 0; v < values; ++v, p += 4) {
    std::swap(p[0], p[3]);
    std::swap(p[1], p[2]);
  }
}

FloatRead read_float_tiff(const std::string& path, bool load_pixels, RawFloatTiff* out, FloatTiffInfo* info,
                          std::string* error) {
  *info = FloatTiffInfo{};
  std::ifstream input = open_input_binary(path);
  if (!input) {
    if (error) *error = "Could not open HDR file: " + path;
    return FloatRead::kError;
  }
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (file.size() < 8) return pass_on(info, "file is too small for a TIFF");
  bool le = false;
  if (file[0] == 'I' && file[1] == 'I') le = true;
  else if (!(file[0] == 'M' && file[1] == 'M')) return pass_on(info, "not a TIFF");
  if (tiff_u16(file.data() + 2, le) != 42) return pass_on(info, "not a classic TIFF");
  info->is_tiff = true;
  info->big_endian = !le;
  const uint32_t ifd = tiff_u32(file.data() + 4, le);
  if (static_cast<size_t>(ifd) + 2 > file.size()) return pass_on(info, "TIFF IFD is past the end of the file");
  const uint16_t ntags = tiff_u16(file.data() + ifd, le);
  if (static_cast<size_t>(ifd) + 2u + static_cast<size_t>(ntags) * 12u > file.size()) {
    return pass_on(info, "TIFF IFD is truncated");
  }

  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t compression = 1;
  uint16_t predictor = 1;
  uint16_t photometric = 2;
  uint16_t samples = 1;
  uint16_t planar = 1;
  uint16_t orientation = 1;
  uint32_t rows_per_strip = 0;
  bool icc_broken = false;
  const uint8_t* bits = nullptr;
  uint32_t bits_count = 0;
  const uint8_t* sample_fmt = nullptr;
  uint32_t sample_fmt_count = 0;
  const uint8_t* strip_off = nullptr;
  uint32_t strip_off_count = 0;
  uint16_t strip_off_type = 4;
  const uint8_t* strip_bytes = nullptr;
  uint32_t strip_bytes_count = 0;
  uint16_t strip_bytes_type = 4;
  bool tiled = false;

  for (uint16_t i = 0; i < ntags; ++i) {
    const uint8_t* entry = file.data() + ifd + 2 + static_cast<size_t>(i) * 12u;
    const uint16_t tag = tiff_u16(entry, le);
    const uint16_t type = tiff_u16(entry + 2, le);
    const uint32_t count = tiff_u32(entry + 4, le);
    if (tag == 256) width = tiff_scalar(entry, le);
    else if (tag == 257) height = tiff_scalar(entry, le);
    else if (tag == 258) {
      bits = tiff_values(file, entry, le, type, count);
      bits_count = count;
    } else if (tag == 259) compression = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 262) photometric = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 273 || tag == 324) {
      if (tag == 324) tiled = true;
      strip_off = tiff_values(file, entry, le, type, count);
      strip_off_count = count;
      strip_off_type = type;
    } else if (tag == 274) orientation = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 277) samples = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 278 || tag == 323) {
      if (tag == 323) tiled = true;
      rows_per_strip = tiff_scalar(entry, le);
    } else if (tag == 279 || tag == 325) {
      if (tag == 325) tiled = true;
      strip_bytes = tiff_values(file, entry, le, type, count);
      strip_bytes_count = count;
      strip_bytes_type = type;
    } else if (tag == 284) planar = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 317) predictor = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 322) tiled = true;
    else if (tag == 339) {
      sample_fmt = tiff_values(file, entry, le, type, count);
      sample_fmt_count = count;
    } else if (tag == 34675 && count > 4) {
      const uint8_t* icc = tiff_values(file, entry, le, 7, count);
      if (!icc) {
        icc_broken = true;
      } else {
        info->has_icc = true;
        info->icc = classify_icc(std::vector<uint8_t>(icc, icc + count));
      }
    }
  }
  info->width = width;
  info->height = height;
  info->compression = compression;
  info->predictor = predictor;

  auto every = [&](const uint8_t* p, uint32_t n, uint16_t want) {
    if (!p || n < samples) return false;
    for (uint32_t s = 0; s < samples; ++s) {
      if (tiff_u16(p + s * 2u, le) != want) return false;
    }
    return true;
  };
  if (tiled) return pass_on(info, "tiled TIFF");
  if (photometric != 2) return pass_on(info, "not RGB");
  if (samples != 3 && samples != 4) return pass_on(info, "not 3 or 4 samples per pixel");
  if (planar != 1) return pass_on(info, "planar TIFF");
  if (orientation > 1) return pass_on(info, "Orientation tag rotates the image");
  if (!every(bits, bits_count, 32) || !every(sample_fmt, sample_fmt_count, 3)) {
    return pass_on(info, "not 32-bit IEEE float samples");
  }
  if (compression != 1 && compression != 8 && compression != 32946) {
    return pass_on(info, "compression is not none or Deflate");
  }
  if (predictor != 1 && predictor != 3) return pass_on(info, "predictor is not 1 or 3");
  if (icc_broken) return pass_on(info, "ICC tag is past the end of the file");
  if (info->has_icc && info->icc.transfer != IccTransfer::kLinear) return pass_on(info, "ICC curve is not linear");
  if (info->has_icc && info->icc.primaries == IccPrimaries::kUnknown) {
    return pass_on(info, "ICC primaries are not Rec.2020, Display P3, or sRGB");
  }
  if (width < 2 || height < 2 || !strip_off || strip_off_count < 1) return pass_on(info, "missing size or strips");
  if (compression != 1 && (!strip_bytes || strip_bytes_count < strip_off_count)) {
    return pass_on(info, "compressed TIFF has no strip byte counts");
  }
  info->supported = true;

  out->width = width;
  out->height = height;
  if (!load_pixels) return FloatRead::kRaw;
  if (rows_per_strip == 0 || rows_per_strip > height) rows_per_strip = height;

  const size_t row_values = static_cast<size_t>(width) * samples;
  const size_t row_bytes = row_values * 4u;
  out->rgb.assign(static_cast<size_t>(width) * height * 3u, 0.f);
  std::vector<uint8_t> strip;
  uint32_t row = 0;
  for (uint32_t s = 0; s < strip_off_count && row < height; ++s) {
    const uint32_t off = tiff_at(strip_off, strip_off_type, s, le);
    const uint32_t rows = std::min(rows_per_strip, height - row);
    const size_t need = static_cast<size_t>(rows) * row_bytes;
    const size_t nbytes = (strip_bytes && s < strip_bytes_count) ? tiff_at(strip_bytes, strip_bytes_type, s, le) : need;
    if (static_cast<size_t>(off) + nbytes > file.size() || (compression == 1 && nbytes < need)) {
      if (error) *error = "HDR TIFF strips are truncated: " + path;
      return FloatRead::kError;
    }
    if (compression == 1) {
      strip.assign(file.begin() + off, file.begin() + off + need);
    } else {
      strip.assign(need, 0);
      mz_ulong len = static_cast<mz_ulong>(need);
      if (mz_uncompress(strip.data(), &len, file.data() + off, static_cast<mz_ulong>(nbytes)) != MZ_OK ||
          len != need) {
        if (error) *error = "HDR TIFF Deflate strip is corrupt: " + path;
        return FloatRead::kError;
      }
    }
    for (uint32_t r = 0; r < rows; ++r) {
      uint8_t* p = strip.data() + static_cast<size_t>(r) * row_bytes;
      if (predictor == 3) undo_float_predictor(p, row_values, samples);
      else if (!le) swap_float_bytes(p, row_values);
      float* dst = out->rgb.data() + static_cast<size_t>(row + r) * width * 3u;
      for (uint32_t x = 0; x < width; ++x) {
        std::memcpy(dst + static_cast<size_t>(x) * 3u, p + static_cast<size_t>(x) * samples * 4u, 12);
      }
    }
    row += rows;
  }
  if (row < height) {
    if (error) *error = "HDR TIFF float strips do not cover the image: " + path;
    return FloatRead::kError;
  }
  if (info->has_icc && info->icc.primaries != IccPrimaries::kRec2020) {
    const bool p3 = info->icc.primaries == IccPrimaries::kDisplayP3;
    for (size_t i = 0; i < out->rgb.size(); i += 3) {
      const LinearRgb src{out->rgb[i], out->rgb[i + 1], out->rgb[i + 2]};
      const LinearRgb rec = p3 ? display_p3_to_rec2020(src) : linear_srgb_to_rec2020(src);
      out->rgb[i] = rec.r;
      out->rgb[i + 1] = rec.g;
      out->rgb[i + 2] = rec.b;
    }
  }
  return FloatRead::kRaw;
}
```

Predictor 3 stores each row as byte planes, most significant byte first, then horizontal byte differences. Undoing it yields host-order floats whatever the file's byte order, so only predictor 1 needs `swap_float_bytes`.

Replace the two public functions after the anonymous namespace with:

```cpp
bool describe_float_tiff(const std::string& path, FloatTiffInfo* info, std::string* error) {
  RawFloatTiff raw;
  return read_float_tiff(path, false, &raw, info, error) != FloatRead::kError;
}

void log_float_tiff_fallback(const std::string& path) {
  FloatTiffInfo info;
  std::string err;
  describe_float_tiff(path, &info, &err);
  std::fprintf(stderr, "HDR TIFF: portable reader passed (%s); using the OS decoder\n",
               info.why.empty() ? err.c_str() : info.why.c_str());
}

int probe_float_tiff_portable(const std::string& path, unsigned* master_w, unsigned* master_h,
                              std::string* error) {
  if (!master_w || !master_h) {
    if (error) *error = "internal: null size output";
    return -1;
  }
  RawFloatTiff raw;
  FloatTiffInfo info;
  const FloatRead kind = read_float_tiff(path, false, &raw, &info, error);
  if (kind == FloatRead::kError) return -1;
  if (kind == FloatRead::kFallback) return 0;
  unsigned w = 0;
  unsigned h = 0;
  even_normalize(raw.width, raw.height, &w, &h);
  if (w < 2 || h < 2) {
    if (error) *error = "Invalid HDR image extent";
    return -1;
  }
  *master_w = w;
  *master_h = h;
  return 1;
}

int load_float_tiff_portable(const std::string& path, RawImageHolder* out, std::string* error,
                             unsigned master_w, unsigned master_h, const CropRect* crop,
                             unsigned dst_w, unsigned dst_h) {
  if (!out) {
    if (error) *error = "internal: null output";
    return -1;
  }
  RawFloatTiff raw;
  FloatTiffInfo info;
  const FloatRead kind = read_float_tiff(path, true, &raw, &info, error);
  if (kind == FloatRead::kError) return -1;
  if (kind == FloatRead::kFallback) return 0;
  out->reset();
  if (!publish_raw(raw, out, error, master_w, master_h, crop, dst_w, dst_h)) return -1;
  return 1;
}
```

- [ ] **Step 4: Call the portable reader from both OS front ends**

In `tools/uhdr_repack/src/tiff_input_win.cpp` and `tools/uhdr_repack/src/tiff_input.mm`:

- In `probe_hdr_tiff_even_size`, replace `probe_identity_rec2020_tiff(path, master_w, master_h, error)` with `probe_float_tiff_portable(path, master_w, master_h, error)`, and after `if (identity > 0) return true;` add `log_float_tiff_fallback(path);`.
- In `load_hdr_tiff_raw`, replace `load_identity_rec2020_tiff(path, out, error, master_w, master_h, crop, dst_w, dst_h)` with `load_float_tiff_portable(path, out, error, master_w, master_h, crop, dst_w, dst_h)`. Leave `if (identity > 0) return true;` and `out->reset();` as they are.

The probe logs the fallback once per encode; the loader does not log again.

In `tools/uhdr_repack/include/tiff_input.h`, replace the `load_hdr_tiff_raw` comment with:

```cpp
/**
 * Load an HDR TIFF into linear Rec.2020 RGBA half-float.
 * load_float_tiff_portable reads float TIFFs (uncompressed or Deflate, linear profile) on both
 * platforms. Anything else is read with Core Image on Mac or WIC on Windows, after one stderr line.
 * When crop is non-null, master_w/master_h must be the even-normalized full frame size.
 */
```

In `tools/uhdr_repack/src/hdr_chart.cpp`, in `reload_identity`, replace `load_identity_rec2020_tiff(path, &hdr, error)` with `load_float_tiff_portable(path, &hdr, error)` and the message `"TIFF was not read as uncompressed linear Rec.2020"` with `"TIFF was not read by the portable reader"`.

- [ ] **Step 5: Add the portable SDR JPEG loader**

`tools/uhdr_repack/include/sdr_jpeg.h`:

```cpp
#pragma once

#include "icc_profile.h"
#include "raw_image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

/** What the portable SDR loader saw. supported=false means an OS decoder reads the file. */
struct SdrJpegInfo {
  bool is_jpeg = false;
  bool supported = false;
  std::string why;
  unsigned width = 0;
  unsigned height = 0;
  bool has_icc = false;
  IccClass icc;
};

bool describe_sdr_jpeg(const std::string& path, SdrJpegInfo* info, std::string* error);

/** libjpeg decode to RGBA8 (A = 255). */
bool decode_jpeg_rgba8(const std::vector<uint8_t>& jpeg, std::vector<uint8_t>& rgba, unsigned* width,
                       unsigned* height, std::string* error);

/** Nearest-neighbor scale of src to master size, then crop out_w×out_h at crop_x, crop_y. */
bool scale_crop_rgba8_buffer(const std::vector<uint8_t>& src, unsigned src_w, unsigned src_h,
                             unsigned master_w, unsigned master_h, unsigned out_w, unsigned out_h,
                             unsigned crop_x, unsigned crop_y, std::vector<uint8_t>& dst,
                             std::string* error);

/**
 * Decode an sRGB-curve JPEG (untagged = sRGB, or tagged sRGB or Display P3 primaries) into
 * Display P3 RGBA8, scaled to master size and cropped. Same code on macOS and Windows.
 * @return 1 loaded, 0 not handled here (caller uses the OS decoder), -1 error.
 */
int load_sdr_jpeg_p3(const std::string& path, unsigned master_w, unsigned master_h, unsigned out_w,
                     unsigned out_h, unsigned crop_x, unsigned crop_y, std::vector<uint8_t>* rgba,
                     std::string* error);

/** Display P3 RGBA8 to a BT.601 4:2:0 libultrahdr image tagged Display P3, sRGB transfer, full range. */
bool rgba_p3_to_sdr_raw(const std::vector<uint8_t>& rgba, unsigned w, unsigned h, RawImageHolder* out,
                        std::string* error);

/** One stderr line saying why the portable loader passed on path. */
void log_sdr_jpeg_fallback(const std::string& path);

}  // namespace uhdr_repack
```

`tools/uhdr_repack/src/sdr_jpeg.cpp`: move `decode_jpeg_rgba8` and `scale_crop_rgba8_buffer` here verbatim from `wic_utils.cpp` (lines 51–135 today), outside any anonymous namespace, and add the rest:

```cpp
#include "sdr_jpeg.h"

#include "color_primaries.h"
#include "jpeg_container.h"
#include "yuv_convert.h"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstring>

extern "C" {
#ifndef _WIN32
#ifndef HAVE_BOOLEAN
#define HAVE_BOOLEAN
typedef int boolean;
#endif
#endif
#include <jpeglib.h>
}

namespace uhdr_repack {

namespace {

struct JpegError {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void jpeg_error_exit(j_common_ptr cinfo) { longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jump, 1); }

bool read_jpeg_header(const std::vector<uint8_t>& jpeg, unsigned* w, unsigned* h, int* color_space) {
  jpeg_decompress_struct cinfo{};
  JpegError jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, jpeg.data(), static_cast<unsigned long>(jpeg.size()));
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  *w = cinfo.image_width;
  *h = cinfo.image_height;
  *color_space = static_cast<int>(cinfo.jpeg_color_space);
  jpeg_destroy_decompress(&cinfo);
  return true;
}

}  // namespace

// decode_jpeg_rgba8 and scale_crop_rgba8_buffer, moved from wic_utils.cpp, go here.

bool describe_sdr_jpeg(const std::string& path, SdrJpegInfo* info, std::string* error) {
  *info = SdrJpegInfo{};
  std::vector<uint8_t> file;
  if (!load_binary_file(path, &file, error)) return false;
  if (file.size() < 4 || file[0] != 0xff || file[1] != 0xd8) {
    info->why = "not a JPEG";
    return true;
  }
  info->is_jpeg = true;
  int color_space = 0;
  if (!read_jpeg_header(file, &info->width, &info->height, &color_space)) {
    info->why = "libjpeg could not read the header";
    return true;
  }
  if (color_space != JCS_YCbCr && color_space != JCS_GRAYSCALE && color_space != JCS_RGB) {
    info->why = "JPEG is CMYK or YCCK";
    return true;
  }
  std::vector<uint8_t> icc;
  std::string icc_err;
  info->has_icc = read_embedded_icc(path, &icc, &icc_err);
  if (info->has_icc) {
    info->icc = classify_icc(icc);
    if (info->icc.transfer != IccTransfer::kSrgb) {
      info->why = "ICC curve is not the sRGB curve";
      return true;
    }
    if (info->icc.primaries != IccPrimaries::kDisplayP3 && info->icc.primaries != IccPrimaries::kSrgb) {
      info->why = "ICC primaries are not Display P3 or sRGB";
      return true;
    }
  }
  info->supported = true;
  return true;
}

int load_sdr_jpeg_p3(const std::string& path, unsigned master_w, unsigned master_h, unsigned out_w,
                     unsigned out_h, unsigned crop_x, unsigned crop_y, std::vector<uint8_t>* rgba,
                     std::string* error) {
  SdrJpegInfo info;
  if (!describe_sdr_jpeg(path, &info, error)) return -1;
  if (!info.supported) return 0;
  std::vector<uint8_t> file;
  if (!load_binary_file(path, &file, error)) return -1;
  std::vector<uint8_t> decoded;
  unsigned w = 0;
  unsigned h = 0;
  if (!decode_jpeg_rgba8(file, decoded, &w, &h, error)) return -1;
  if (!scale_crop_rgba8_buffer(decoded, w, h, master_w, master_h, out_w, out_h, crop_x, crop_y, *rgba, error)) {
    return -1;
  }
  if (info.icc.primaries != IccPrimaries::kDisplayP3) {
    srgb_rgba8888_to_display_p3(rgba->data(), static_cast<size_t>(out_w) * out_h);
  }
  return 1;
}

bool rgba_p3_to_sdr_raw(const std::vector<uint8_t>& rgba, unsigned w, unsigned h, RawImageHolder* out,
                        std::string* error) {
  uint8_t* py = nullptr;
  uint8_t* pu = nullptr;
  uint8_t* pv = nullptr;
  if (!rgba8888_to_yuv420_bt601(rgba.data(), w, h, &py, &pu, &pv, error)) return false;
  out->reset();
  uhdr_raw_image_t& r = out->ref();
  std::memset(&r, 0, sizeof(r));
  r.fmt = UHDR_IMG_FMT_12bppYCbCr420;
  r.cg = UHDR_CG_DISPLAY_P3;
  r.ct = UHDR_CT_SRGB;
  r.range = UHDR_CR_FULL_RANGE;
  r.w = w;
  r.h = h;
  r.planes[UHDR_PLANE_Y] = py;
  r.planes[UHDR_PLANE_U] = pu;
  r.planes[UHDR_PLANE_V] = pv;
  r.stride[UHDR_PLANE_Y] = w;
  r.stride[UHDR_PLANE_U] = w / 2;
  r.stride[UHDR_PLANE_V] = w / 2;
  return true;
}

void log_sdr_jpeg_fallback(const std::string& path) {
  SdrJpegInfo info;
  std::string err;
  describe_sdr_jpeg(path, &info, &err);
  std::fprintf(stderr, "SDR base: portable loader passed (%s); using the OS decoder\n",
               info.why.empty() ? err.c_str() : info.why.c_str());
}

}  // namespace uhdr_repack
```

Replace the placeholder comment line with the two moved functions. The moved `decode_jpeg_rgba8` uses `JpegError` and `jpeg_error_exit` from the anonymous namespace above it.

- [ ] **Step 6: Point WIC at the moved functions and classify its profiles**

In `tools/uhdr_repack/src/wic_utils.cpp`:

- Add `#include "icc_profile.h"` and `#include "sdr_jpeg.h"`.
- Delete the definitions of `decode_jpeg_rgba8` and `scale_crop_rgba8_buffer`. The calls in `managed_rgba8` and at the other `decode_jpeg_rgba8` call site resolve to `uhdr_repack::` unqualified.
- Delete `icc_bytes_look_like_display_p3`. In `frame_is_display_p3`, replace its last line with:

```cpp
  profile.resize(actual_bytes);
  return classify_icc(profile).primaries == IccPrimaries::kDisplayP3;
```

`JpegError` and `jpeg_error_exit` in `wic_utils.cpp` stay; `encode_jpeg_from_rgba8` still uses them.

- [ ] **Step 7: Call the portable loader from both SDR front ends**

Replace the body of `load_sdr_base_raw` in `tools/uhdr_repack/src/sdr_input_win.cpp`, from `std::vector<uint8_t> rgba;` to the end of the function, with:

```cpp
  std::vector<uint8_t> rgba;
  const int portable = load_sdr_jpeg_p3(path, master_width, master_height, out_w, out_h, crop_x, crop_y,
                                        &rgba, error);
  if (portable < 0) return false;
  if (portable > 0) return rgba_p3_to_sdr_raw(rgba, out_w, out_h, out, error);
  log_sdr_jpeg_fallback(path);
  if (!wic::decode_scale_crop_to_rgba8(path, master_width, master_height, out_w, out_h, crop_x,
                                       crop_y, rgba, error, wic::Rgba8Space::DisplayP3)) {
    return false;
  }
  return rgba_p3_to_sdr_raw(rgba, out_w, out_h, out, error);
}
```

Add `#include "sdr_jpeg.h"`; the `yuv_convert.h` include can go.

In `tools/uhdr_repack/src/sdr_input.mm`, add `#include "sdr_jpeg.h"` and `#include <vector>`. Directly before `@autoreleasepool {` insert:

```cpp
  {
    std::vector<uint8_t> rgba;
    const unsigned crop_x = crop ? crop->x : 0;
    const unsigned crop_y = crop ? crop->y : 0;
    const int portable = load_sdr_jpeg_p3(path, master_width, master_height, out_w, out_h, crop_x, crop_y,
                                          &rgba, error);
    if (portable < 0) return false;
    if (portable > 0) return rgba_p3_to_sdr_raw(rgba, out_w, out_h, out, error);
    log_sdr_jpeg_fallback(path);
  }
```

Then replace everything from `    uint8_t* py = nullptr;` to the end of the function with:

```objc
    std::vector<uint8_t> rgba(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + nbytes);
    std::free(buf);
    return rgba_p3_to_sdr_raw(rgba, out_w, out_h, out, error);
  }
}
```

- [ ] **Step 8: Add `--describe-input`**

In `tools/uhdr_repack/include/cli.h`, add `int describe_input_main(int argc, char** argv);`.

`tools/uhdr_repack/src/cli_describe.cpp`:

```cpp
#include "cli.h"

#include "path_io.h"
#include "sdr_jpeg.h"
#include "tiff_float.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace uhdr_repack {

int describe_input_main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "--describe-input requires an image path\n";
    return 1;
  }
  const std::string path = argv[2];
  bool require_portable = false;
  bool skip_if_missing = false;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--require-portable") == 0) require_portable = true;
    else if (std::strcmp(argv[i], "--skip-if-missing") == 0) skip_if_missing = true;
    else {
      std::cerr << "unknown --describe-input argument: " << argv[i] << "\n";
      return 1;
    }
  }
  if (!std::filesystem::exists(path_from_utf8(path))) {
    if (skip_if_missing) {
      std::cout << "SKIP: missing " << path << "\n";
      return 77;
    }
    std::cerr << "missing " << path << "\n";
    return 1;
  }
  std::string err;
  FloatTiffInfo tiff;
  if (!describe_float_tiff(path, &tiff, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  bool supported = false;
  std::string why;
  if (tiff.is_tiff) {
    std::cout << "TIFF " << tiff.width << "x" << tiff.height << " compression=" << tiff.compression
              << " predictor=" << tiff.predictor << (tiff.big_endian ? " MM" : " II")
              << " icc=" << (tiff.has_icc ? icc_class_name(tiff.icc) : std::string("none"));
    supported = tiff.supported;
    why = tiff.why;
  } else {
    SdrJpegInfo jpeg;
    if (!describe_sdr_jpeg(path, &jpeg, &err)) {
      std::cerr << err << "\n";
      return 1;
    }
    if (!jpeg.is_jpeg) {
      std::cerr << path << " is not a TIFF or JPEG\n";
      return 1;
    }
    std::cout << "JPEG " << jpeg.width << "x" << jpeg.height
              << " icc=" << (jpeg.has_icc ? icc_class_name(jpeg.icc) : std::string("none (read as sRGB)"));
    supported = jpeg.supported;
    why = jpeg.why;
  }
  std::cout << (supported ? std::string(" reader=portable") : " reader=os (" + why + ")") << "\n";
  return require_portable && !supported ? 1 : 0;
}

}  // namespace uhdr_repack
```

In `main.cpp`, after the `--inspect` block:

```cpp
  if (std::strcmp(argv[1], "--describe-input") == 0) {
    return describe_input_main(argc, argv);
  }
```

In `print_usage`, add after the `--inspect` line:

```text
      << "  uhdr_repack --describe-input <tif-or-jpg> [--require-portable] [--skip-if-missing]\n"
```

- [ ] **Step 9: Build and prove the portable path runs**

Reconfigure, then:

```text
cmake --build tools/uhdr_repack/build --target uhdr_repack
uhdr_repack --write-hdr-chart tools/uhdr_repack/build/test-out/hdr-chart
uhdr_repack --describe-input tools/uhdr_repack/build/test-out/hdr-chart/hdr-chart.tif --require-portable
uhdr_repack --describe-input tools/uhdr_repack/build/test-out/hdr-chart/sdr-chart.jpg --require-portable
uhdr_repack --check-hdr-chart tools/uhdr_repack/build/test-out/hdr-chart
```

Expected, in order:

- exit 0, same last line as Task 1;
- `TIFF 1440x1920 compression=1 predictor=1 II icc=Rec.2020 / linear reader=portable`, exit 0;
- `JPEG 1440x1920 icc=Display P3 / sRGB curve reader=portable`, exit 0;
- `HDR_CHART encoder PASS ...`, exit 0, with no `using the OS decoder` line on stderr.

In a GUI build, also run the existing `uhdr_repack --self-test`. Expected: exit 0.

- [ ] **Step 10: Describe a real Lightroom TIFF**

If `test/ui/fixtures/DSC02993.tif` exists (a Lightroom HDR export kept with "Keep HDR TIFF temp files"), run:

```text
uhdr_repack --describe-input test/ui/fixtures/DSC02993.tif --require-portable
```

Expected: `reader=portable` and exit 0. If it prints `reader=os (<why>)`, stop and report the full line. It names the Lightroom TIFF feature the reader lacks, and the fix belongs in `read_float_tiff`, not in a looser test. If the fixture is absent, note that in the commit message; Task 8's manual check covers it.

- [ ] **Step 11: Commit**

```text
git add tools/uhdr_repack/CMakeLists.txt tools/uhdr_repack/include/tiff_float.h tools/uhdr_repack/src/tiff_float.cpp tools/uhdr_repack/include/tiff_input.h tools/uhdr_repack/src/tiff_input_win.cpp tools/uhdr_repack/src/tiff_input.mm tools/uhdr_repack/include/sdr_jpeg.h tools/uhdr_repack/src/sdr_jpeg.cpp tools/uhdr_repack/src/sdr_input_win.cpp tools/uhdr_repack/src/sdr_input.mm tools/uhdr_repack/src/wic_utils.cpp tools/uhdr_repack/src/cli_describe.cpp tools/uhdr_repack/include/cli.h tools/uhdr_repack/src/main.cpp tools/uhdr_repack/src/cli_encode.cpp tools/uhdr_repack/src/hdr_chart.cpp
git commit -m "Read Lightroom's ZIP float TIFF and P3 JPEG with the same code on macOS and Windows."
```

---

### Task 3: A chart that matches Lightroom's TIFF and survives Instagram crops

**Files:**
- Modify: `tools/uhdr_repack/src/hdr_chart.cpp`, `tools/uhdr_repack/include/hdr_chart.h`
- Modify: `tools/uhdr_repack/src/main.cpp`, `tools/uhdr_repack/src/cli_encode.cpp` (`print_usage`)
- Regenerate and commit: `test/hdr-chart/hdr-chart.tif`, `test/hdr-chart/sdr-chart.jpg`, `test/hdr-chart/chart-manifest.json`

**Interfaces:**
- Consumes: `describe_float_tiff`, `describe_sdr_jpeg` (Task 2), `compute_slices` (`slice_plan.h`), miniz `mz_compress2`, `mz_compressBound`.
- Produces:
  - `bool frame_for(int chart_w, int chart_h, SliceAspect aspect, float crop_offset, Rect* frame, std::string* error);` in the anonymous namespace (Task 6 reuses it).
  - `int check_hdr_chart_assets_main(const std::string& committed_dir, const std::string& fresh_dir);`
  - Manifest ids for the new hue stops, `hue/<H>/-4` and `hue/<H>/-2`. Existing ids are unchanged.

- [ ] **Step 1: Move the neutral row and add dark hue stops**

In `tools/uhdr_repack/src/hdr_chart.cpp`, replace the A and B constants:

```cpp
constexpr int kAX = 80, kAY = 62, kAPatch = 88, kAPatchH = 48, kAGap = 6;
constexpr int kBX = 80, kBY = 128, kBPatch = 56, kBGap = 6;
constexpr int kHueStops[7] = {-4, -2, 0, 1, 2, 3, 4};
```

Add to the `static_assert` list:

```cpp
static_assert(kAY + kAPatchH <= kBY - 18, "neutral row overlaps the hue labels");
static_assert(kBX + 7 * kBPatch + 6 * kBGap <= kCX - 28, "hue grid overlaps the gamut row labels");
```

In `build_samples`, change the neutral `push_flat` rectangle to `{kAX + col * (kAPatch + kAGap), kAY, kAPatch, kAPatchH}`. Replace the hue loop body with:

```cpp
  for (int row = 0; row < 12; ++row) {
    const LinearRgb unit = p3_edge(row * 30);
    for (int col = 0; col < 7; ++col) {
      ChartSample s;
      s.id = std::string("hue/") + hue_labels[row] + "/" + stop_tag(kHueStops[col]);
      s.group = "hue";
      s.expected = at_stop(unit, kHueStops[col]);
      push_flat(out, std::move(s),
                {kBX + col * (kBPatch + kBGap), kBY + row * (kBPatch + kBGap), kBPatch, kBPatch});
    }
  }
```

In `draw_labels`, draw the neutral labels at `kAY - 18` instead of `4` (both the `"STOPS"` call and the per-stop call). Replace the hue column label loop with:

```cpp
  for (int col = 0; col < 7; ++col) {
    draw_text_centered(hdr, sdr, kBX + col * (kBPatch + kBGap), kBY - 16, kBPatch, stop_tag(kHueStops[col]).c_str());
  }
```

The neutral samples now sit at y 74–98, inside the 4:5 crop (y 60–1860).

- [ ] **Step 2: Write the TIFF the way Lightroom does**

Add `#include <miniz.h>`, `#include "sdr_jpeg.h"`, and `#include "slice_plan.h"` to `hdr_chart.cpp`. Before `write_float_tiff`, add:

```cpp
void apply_float_predictor(uint8_t* row, size_t values, uint32_t stride) {
  const size_t bytes = values * 4u;
  std::vector<uint8_t> planes(bytes);
  for (size_t v = 0; v < values; ++v) {
    for (size_t b = 0; b < 4; ++b) planes[(3 - b) * values + v] = row[v * 4u + b];
  }
  for (size_t i = bytes; i-- > stride;) planes[i] = static_cast<uint8_t>(planes[i] - planes[i - stride]);
  std::memcpy(row, planes.data(), bytes);
}
```

Replace `write_float_tiff` with:

```cpp
bool write_float_tiff(const std::string& path, const std::vector<float>& rgb, const std::vector<uint8_t>& icc,
                      std::string* error) {
  const uint32_t nstrips = static_cast<uint32_t>(kHeight / kStripRows);
  const size_t row_values = static_cast<size_t>(kWidth) * 3u;
  const size_t strip_raw = static_cast<size_t>(kStripRows) * row_values * 4u;
  if (rgb.size() < static_cast<size_t>(kHeight) * row_values) {
    if (error) *error = "internal: HDR buffer does not cover the TIFF";
    return false;
  }
  std::vector<std::vector<uint8_t>> strips(nstrips);
  std::vector<uint8_t> raw(strip_raw);
  for (uint32_t s = 0; s < nstrips; ++s) {
    std::memcpy(raw.data(), rgb.data() + static_cast<size_t>(s) * kStripRows * row_values, strip_raw);
    for (int r = 0; r < kStripRows; ++r) {
      apply_float_predictor(raw.data() + static_cast<size_t>(r) * row_values * 4u, row_values, 3);
    }
    mz_ulong len = mz_compressBound(static_cast<mz_ulong>(strip_raw));
    strips[s].resize(len);
    if (mz_compress2(strips[s].data(), &len, raw.data(), static_cast<mz_ulong>(strip_raw), 6) != MZ_OK) {
      if (error) *error = "Deflate failed on TIFF strip " + std::to_string(s);
      return false;
    }
    strips[s].resize(len);
  }

  constexpr uint32_t ntags = 18;
  constexpr uint32_t ifd = 8;
  constexpr uint32_t after = ifd + 2 + ntags * 12 + 4;
  uint32_t cursor = (after + 3u) & ~3u;
  const uint32_t bits_off = cursor;
  cursor += 8;
  const uint32_t fmt_off = cursor;
  cursor += 8;
  const uint32_t xres_off = cursor;
  cursor += 8;
  const uint32_t yres_off = cursor;
  cursor += 8;
  const char* software = "uhdr_repack";
  const uint32_t software_len = static_cast<uint32_t>(std::strlen(software) + 1);
  const uint32_t soft_off = cursor;
  cursor += software_len;
  cursor = (cursor + 3u) & ~3u;
  const uint32_t strips_off = cursor;
  cursor += nstrips * 4u;
  const uint32_t counts_off = cursor;
  cursor += nstrips * 4u;
  const uint32_t icc_off = cursor;
  cursor += static_cast<uint32_t>(icc.size());
  cursor = (cursor + 3u) & ~3u;
  const uint32_t data_off = cursor;
  size_t total = data_off;
  for (const auto& strip : strips) total += strip.size();

  std::vector<uint8_t> file(total, 0);
  file[0] = 'I';
  file[1] = 'I';
  put_u16(file, 2, 42);
  put_u32(file, 4, ifd);
  put_u16(file, ifd, static_cast<uint16_t>(ntags));
  size_t e = ifd + 2;
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
    put_u16(file, e, tag);
    put_u16(file, e + 2, type);
    put_u32(file, e + 4, count);
    put_u32(file, e + 8, value);
    e += 12;
  };
  entry(256, 3, 1, static_cast<uint32_t>(kWidth));
  entry(257, 3, 1, static_cast<uint32_t>(kHeight));
  entry(258, 3, 3, bits_off);
  entry(259, 3, 1, 8);
  entry(262, 3, 1, 2);
  entry(273, 4, nstrips, strips_off);
  entry(274, 3, 1, 1);
  entry(277, 3, 1, 3);
  entry(278, 3, 1, static_cast<uint32_t>(kStripRows));
  entry(279, 4, nstrips, counts_off);
  entry(282, 5, 1, xres_off);
  entry(283, 5, 1, yres_off);
  entry(284, 3, 1, 1);
  entry(296, 3, 1, 2);
  entry(305, 2, software_len, soft_off);
  entry(317, 3, 1, 3);
  entry(339, 3, 3, fmt_off);
  entry(34675, 7, static_cast<uint32_t>(icc.size()), icc_off);
  put_u32(file, e, 0);
  put_u16(file, bits_off, 32);
  put_u16(file, bits_off + 2, 32);
  put_u16(file, bits_off + 4, 32);
  put_u16(file, fmt_off, 3);
  put_u16(file, fmt_off + 2, 3);
  put_u16(file, fmt_off + 4, 3);
  put_u32(file, xres_off, 72);
  put_u32(file, xres_off + 4, 1);
  put_u32(file, yres_off, 72);
  put_u32(file, yres_off + 4, 1);
  std::memcpy(file.data() + soft_off, software, software_len);
  std::memcpy(file.data() + icc_off, icc.data(), icc.size());
  uint32_t at = data_off;
  for (uint32_t s = 0; s < nstrips; ++s) {
    put_u32(file, strips_off + s * 4u, at);
    put_u32(file, counts_off + s * 4u, static_cast<uint32_t>(strips[s].size()));
    std::memcpy(file.data() + at, strips[s].data(), strips[s].size());
    at += static_cast<uint32_t>(strips[s].size());
  }

  std::ofstream out = open_output_binary(path);
  if (!out) {
    if (error) *error = "could not write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  if (!out) {
    if (error) *error = "could not finish " + path;
    return false;
  }
  return true;
}
```

- [ ] **Step 3: Add `frame_for` and make the writer prove its own file**

In the anonymous namespace, after `is_tiff_path`, add:

```cpp
bool frame_for(int chart_w, int chart_h, SliceAspect aspect, float crop_offset, Rect* frame, std::string* error) {
  *frame = {0, 0, chart_w, chart_h};
  if (aspect == SliceAspect::kNone) return true;
  std::vector<CropRect> tiles;
  if (!compute_slices(static_cast<unsigned>(chart_w), static_cast<unsigned>(chart_h), aspect, &tiles, error,
                      crop_offset, 1) ||
      tiles.empty()) {
    if (error && error->empty()) *error = "no crop fits the chart";
    return false;
  }
  *frame = {static_cast<int>(tiles[0].x), static_cast<int>(tiles[0].y), static_cast<int>(tiles[0].w),
            static_cast<int>(tiles[0].h)};
  return true;
}

bool inside(const ChartSample& s, const Rect& f) {
  return s.x >= f.x && s.y >= f.y && s.x + s.w <= f.x + f.w && s.y + s.h <= f.y + f.h;
}
```

In `write_hdr_chart_main`, delete the `reload_identity` block (the portable reader is now the production reader, already checked by the `load_tiff_rec2020` block). After the embedded-profile check, insert:

```cpp
  FloatTiffInfo tiff_info;
  SdrJpegInfo jpeg_info;
  if (!describe_float_tiff(tiff, &tiff_info, &err) || !describe_sdr_jpeg(jpeg, &jpeg_info, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  if (!tiff_info.supported || tiff_info.compression != 8 || tiff_info.predictor != 3) {
    std::cerr << "chart TIFF must be Deflate + predictor 3 and read by the portable reader: " << tiff_info.why << "\n";
    return 1;
  }
  if (!jpeg_info.supported) {
    std::cerr << "chart JPEG must be read by the portable loader: " << jpeg_info.why << "\n";
    return 1;
  }
  Rect feed;
  if (!frame_for(kWidth, kHeight, SliceAspect::k4x5, 0.5f, &feed, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  for (const ChartSample& s : samples) {
    if (s.gate_encoder && !inside(s, feed)) {
      std::cerr << "gated sample " << s.id << " is outside the Instagram 4:5 crop\n";
      return 1;
    }
  }
```

Delete `reload_identity`; nothing calls it now. Change the last `std::cout` line to:

```cpp
  std::cout << kWidth << "x" << kHeight
            << " Deflate float TIFF, embedded profiles valid, portable readers match the manifest,"
               " gated samples fit 4:5\n";
```

- [ ] **Step 4: Add the committed-asset comparer**

Declare in `hdr_chart.h`:

```cpp
/** Fail unless hdr-chart.tif and chart-manifest.json in both directories match byte for byte. */
int check_hdr_chart_assets_main(const std::string& committed_dir, const std::string& fresh_dir);
```

In `hdr_chart.cpp`, add `same_bytes` inside the existing anonymous namespace:

```cpp
bool same_bytes(const std::string& a, const std::string& b, std::string* error) {
  std::ifstream ia = open_input_binary(a);
  std::ifstream ib = open_input_binary(b);
  if (!ia || !ib) {
    if (error) *error = "could not open " + a + " and " + b;
    return false;
  }
  const std::vector<char> ba((std::istreambuf_iterator<char>(ia)), {});
  const std::vector<char> bb((std::istreambuf_iterator<char>(ib)), {});
  if (ba != bb) {
    if (error) *error = a + " and " + b + " differ (" + std::to_string(ba.size()) + " vs " + std::to_string(bb.size()) + " bytes)";
    return false;
  }
  return true;
}
```

After `check_hdr_chart_file_main`, outside the anonymous namespace:

```cpp
int check_hdr_chart_assets_main(const std::string& committed_dir, const std::string& fresh_dir) {
  for (const char* name : {"hdr-chart.tif", "chart-manifest.json"}) {
    std::string err;
    if (!same_bytes(join_dir(committed_dir, name), join_dir(fresh_dir, name), &err)) {
      std::cerr << err << "\nCommitted chart is stale. Run --write-hdr-chart test/hdr-chart.\n";
      return 1;
    }
  }
  std::cout << "HDR_CHART assets match\n";
  return 0;
}
```

In `main.cpp`, after the `--check-hdr-chart-file` block:

```cpp
  if (std::strcmp(argv[1], "--check-hdr-chart-assets") == 0) {
    if (argc < 4) {
      std::cerr << "--check-hdr-chart-assets requires <committed-dir> <fresh-dir>\n";
      print_usage();
      return 1;
    }
    return check_hdr_chart_assets_main(argv[2], argv[3]);
  }
```

In `print_usage`, add after the chart lines:

```text
      << "  uhdr_repack --check-hdr-chart-assets <committed-dir> <fresh-dir>\n"
```

- [ ] **Step 5: Build, write, and inspect**

```text
cmake --build tools/uhdr_repack/build --target uhdr_repack
uhdr_repack --write-hdr-chart tools/uhdr_repack/build/test-out/hdr-chart
uhdr_repack --describe-input tools/uhdr_repack/build/test-out/hdr-chart/hdr-chart.tif --require-portable
uhdr_repack --check-hdr-chart tools/uhdr_repack/build/test-out/hdr-chart
```

Expected:

- exit 0 and the new last line; the manifest's sample count is exactly 24 higher than before this task (12 hues × 2 new stops);
- `TIFF 1440x1920 compression=8 predictor=3 II icc=Rec.2020 / linear reader=portable`;
- `HDR_CHART encoder PASS`, with `hue/*/-4` and `hue/*/-2` rows in the table.

- [ ] **Step 6: Regenerate the committed chart and compare**

```text
uhdr_repack --write-hdr-chart test/hdr-chart
uhdr_repack --check-hdr-chart-assets test/hdr-chart tools/uhdr_repack/build/test-out/hdr-chart
```

Expected: `HDR_CHART assets match`, exit 0. Open `test/hdr-chart/hdr-chart.tif` in Lightroom: it imports without a profile warning, and the neutral row is fully visible under the 4:5 crop overlay.

- [ ] **Step 7: Commit**

```text
git add tools/uhdr_repack/src/hdr_chart.cpp tools/uhdr_repack/include/hdr_chart.h tools/uhdr_repack/src/main.cpp tools/uhdr_repack/src/cli_encode.cpp test/hdr-chart/hdr-chart.tif test/hdr-chart/sdr-chart.jpg test/hdr-chart/chart-manifest.json
git commit -m "Write the chart as a Deflate float TIFF, add dark hue stops, keep gated samples inside 4:5."
```

---

### Task 4: One preset table for the editor, the CLI, and the tests

**Files:**
- Create: `tools/uhdr_repack/include/encode_presets.h`, `tools/uhdr_repack/src/encode_presets.cpp`
- Modify: `tools/uhdr_repack/CMakeLists.txt`, `tools/uhdr_repack/include/cli.h`, `tools/uhdr_repack/src/cli_encode.cpp`
- Modify: `tools/uhdr_repack/src/gui/gui_bridge.cpp`, `tools/uhdr_repack/web/app.js`, `tools/uhdr_repack/src/gui/gui_self_test.cpp`

**Interfaces:**
- Consumes: `EncodeOptions` (`uhdr_encode.h`), `EncodeRequest` (`encode_engine.h`), `parse_slice_aspect`.
- Produces:
  - `EncodeOptions color_map_preset();` returns `EncodeOptions{}`.
  - `EncodeOptions mono_map_preset();`
  - `bool apply_encode_preset(const std::string& name, EncodeOptions* opt);` for `"color"` or `"mono"`. Sets the seven delivery fields and leaves `gainmap_in`, `watermark_config`, `metadata_patch`, and `sdr_jpeg` alone.
  - `nlohmann::json encode_options_ui_json(const EncodeOptions& opt);` with the camelCase keys `baseQuality`, `gainmapQuality`, `gainmapScale`, `minBoost`, `maxBoost`, `displayPeak`, `monochromeGainmap`.
  - `int parse_encode_flag(int argc, char** argv, int* i, EncodeRequest* req);` returns 1 when it consumed `argv[*i]` and its value, 0 when the flag is not an encode option, −1 for a bad value (message on stderr).
  - The web `"settings"` message gains `presets: {color: {...}, mono: {...}}`.

- [ ] **Step 1: Show the flag is missing**

```text
uhdr_repack --hdr-tiff test/hdr-chart/hdr-chart.tif --base test/hdr-chart/sdr-chart.jpg --out tools/uhdr_repack/build/test-out/mono.jpg --preset mono
```

Expected: `unknown argument: --preset`, usage text, exit 1.

- [ ] **Step 2: Add the table**

`tools/uhdr_repack/include/encode_presets.h`:

```cpp
#pragma once

#include "uhdr_encode.h"

#include <nlohmann/json.hpp>

#include <string>

namespace uhdr_repack {

/** Color map card: the session default. */
EncodeOptions color_map_preset();

/** Mono map card. */
EncodeOptions mono_map_preset();

/** Copy the delivery fields of "color" or "mono" into opt. Returns false for other names. */
bool apply_encode_preset(const std::string& name, EncodeOptions* opt);

/** Delivery fields with the web UI's camelCase keys. */
nlohmann::json encode_options_ui_json(const EncodeOptions& opt);

}  // namespace uhdr_repack
```

`tools/uhdr_repack/src/encode_presets.cpp`:

```cpp
#include "encode_presets.h"

namespace uhdr_repack {

EncodeOptions color_map_preset() { return EncodeOptions{}; }

EncodeOptions mono_map_preset() {
  EncodeOptions o;
  o.gainmap_scale = 2;
  o.max_content_boost = 4.92f;
  o.target_display_peak_nits = 1000.0f;
  o.monochrome_gainmap = true;
  return o;
}

bool apply_encode_preset(const std::string& name, EncodeOptions* opt) {
  EncodeOptions p;
  if (name == "color") p = color_map_preset();
  else if (name == "mono") p = mono_map_preset();
  else return false;
  opt->base_quality = p.base_quality;
  opt->gainmap_quality = p.gainmap_quality;
  opt->gainmap_scale = p.gainmap_scale;
  opt->min_content_boost = p.min_content_boost;
  opt->max_content_boost = p.max_content_boost;
  opt->target_display_peak_nits = p.target_display_peak_nits;
  opt->monochrome_gainmap = p.monochrome_gainmap;
  return true;
}

nlohmann::json encode_options_ui_json(const EncodeOptions& o) {
  return {{"baseQuality", o.base_quality},
          {"gainmapQuality", o.gainmap_quality},
          {"gainmapScale", o.gainmap_scale},
          {"minBoost", o.min_content_boost},
          {"maxBoost", o.max_content_boost},
          {"displayPeak", o.target_display_peak_nits},
          {"monochromeGainmap", o.monochrome_gainmap}};
}

}  // namespace uhdr_repack
```

Add `src/encode_presets.cpp` to `_UHDR_CORE_SOURCES`.

- [ ] **Step 3: Share one flag parser and add `--preset`**

In `tools/uhdr_repack/include/cli.h`, add before the functions:

```cpp
struct EncodeRequest;

/**
 * Consume the encode option at argv[*i] (and its value) into req, advancing *i.
 * Returns 1 consumed, 0 not an encode option, -1 bad value (message on stderr).
 */
int parse_encode_flag(int argc, char** argv, int* i, EncodeRequest* req);
```

In `tools/uhdr_repack/src/cli_encode.cpp`, add `#include "encode_presets.h"`, then add this function after `print_usage`. It carries every option branch `cli_encode_main` had, plus `--preset`:

```cpp
int parse_encode_flag(int argc, char** argv, int* i, EncodeRequest* req) {
  const std::string a = argv[*i];
  if (*i + 1 >= argc) {
    if (a == "--monochrome-gainmap") {
      req->options.monochrome_gainmap = true;
      return 1;
    }
    return 0;
  }
  const char* v = argv[*i + 1];
  auto bad = [](const char* msg) {
    std::cerr << msg << "\n";
    return -1;
  };
  int n = 0;
  float f = 0.f;
  if (a == "--preset") {
    if (!apply_encode_preset(v, &req->options)) return bad("bad --preset (use color or mono)");
  } else if (a == "--slice-aspect") {
    if (!parse_slice_aspect(v, &req->slice_aspect)) return bad("bad --slice-aspect (use none, 1x1, 4x5, 3x4, or 191x100)");
  } else if (a == "--slice-count") {
    if (std::strcmp(v, "max") == 0 || std::strcmp(v, "all") == 0) req->slice_count = 0;
    else if (parse_int(v, &n)) req->slice_count = static_cast<unsigned>(n);
    else return bad("bad --slice-count (use a non-negative integer or max)");
  } else if (a == "--crop-offset") {
    if (!parse_float(v, &f) || f < 0.f || f > 1.f) return bad("bad --crop-offset (use 0..1)");
    req->crop_offset = f;
  } else if (a == "--out-width") {
    if (!parse_int(v, &n)) return bad("bad --out-width");
    req->output_width = static_cast<unsigned>(n);
  } else if (a == "--base-quality") {
    if (!parse_int(v, &n)) return bad("bad --base-quality");
    req->options.base_quality = n;
  } else if (a == "--gainmap-quality") {
    if (!parse_int(v, &n)) return bad("bad --gainmap-quality");
    req->options.gainmap_quality = n;
  } else if (a == "--gainmap-scale") {
    if (!parse_int(v, &n)) return bad("bad --gainmap-scale");
    req->options.gainmap_scale = n;
  } else if (a == "--min-content-boost") {
    if (!parse_float(v, &f)) return bad("bad --min-content-boost");
    req->options.min_content_boost = f;
  } else if (a == "--max-content-boost") {
    if (!parse_float(v, &f)) return bad("bad --max-content-boost");
    req->options.max_content_boost = f;
  } else if (a == "--target-display-peak") {
    if (!parse_float(v, &f)) return bad("bad --target-display-peak");
    req->options.target_display_peak_nits = f;
  } else if (a == "--gainmap-in") {
    req->gainmap_in = v;
  } else if (a == "--watermark-config") {
    req->watermark_config = v;
  } else if (a == "--metadata-patch") {
    req->metadata_patch = v;
  } else if (a == "--monochrome-gainmap") {
    req->options.monochrome_gainmap = true;
    return 1;
  } else {
    return 0;
  }
  ++*i;
  return 1;
}
```

Replace the loop in `cli_encode_main` with:

```cpp
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--hdr-tiff" && i + 1 < argc) {
      req.hdr_tiff = argv[++i];
    } else if (a == "--base" && i + 1 < argc) {
      req.base_path = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      req.out_path = argv[++i];
    } else {
      const int used = parse_encode_flag(argc, argv, &i, &req);
      if (used < 0) return 1;
      if (used == 0) {
        std::cerr << "unknown argument: " << a << "\n";
        print_usage();
        return 1;
      }
    }
  }
```

In `print_usage`, add as the first encode option:

```text
      << "  --preset <color|mono>          editor delivery preset; later options override it\n"
```

- [ ] **Step 4: Send the presets from C++ to the editor**

In `tools/uhdr_repack/src/gui/gui_bridge.cpp`, add `#include "encode_presets.h"`. In the `type == "ready"` branch, replace

```cpp
    json settings;
    settings["type"] = "settings";
    settings["baseQuality"] = opt.base_quality;
    settings["gainmapQuality"] = opt.gainmap_quality;
    settings["gainmapScale"] = opt.gainmap_scale;
    settings["minBoost"] = opt.min_content_boost;
    settings["maxBoost"] = opt.max_content_boost;
    settings["displayPeak"] = opt.target_display_peak_nits;
    settings["monochromeGainmap"] = opt.monochrome_gainmap;
```

with

```cpp
    json settings = encode_options_ui_json(opt);
    settings["type"] = "settings";
    settings["presets"] = {{"color", encode_options_ui_json(color_map_preset())},
                           {"mono", encode_options_ui_json(mono_map_preset())}};
```

- [ ] **Step 5: Stop hard-coding presets in the web UI**

In `tools/uhdr_repack/web/app.js`:

1. Replace the two literal blocks `const COLOR_MAP = { ... };` and `const MONO_MAP = { ... };` with:

```js
  const COLOR_MAP = {};
  const MONO_MAP = {};
```

2. In `state.settings`, replace the seven `COLOR_MAP.*` initializers with:

```js
      baseQuality: null,
      gainmapQuality: null,
      gainmapScale: null,
      minBoost: null,
      maxBoost: null,
      displayPeak: null,
      monochromeGainmap: false,
```

3. At the top of `applySettings(msg)`:

```js
    if (msg.presets) {
      Object.assign(COLOR_MAP, msg.presets.color);
      Object.assign(MONO_MAP, msg.presets.mono);
    }
```

4. In `isColorProfile`, replace `return (q === 95 && g === 95) || (q === 85 && g === 85);` with:

```js
    return (q === COLOR_MAP.baseQuality && g === COLOR_MAP.gainmapQuality) || (q === 85 && g === 85);
```

5. In `syncDeliveryUi`, replace the two preset hint strings with:

```js
      document.getElementById("delivery-hint").textContent =
        `Full-resolution RGB Display P3 gain map with a ${formatBoost(COLOR_MAP.maxBoost)} boost. Highlight color stays in the HDR layer; pixel size is unchanged.`;
```

```js
      document.getElementById("delivery-hint").textContent =
        `Half-resolution luma gain map with boost capped at ${formatBoost(MONO_MAP.maxBoost)}. HDR adds brightness only, not color, and the map is smaller.`;
```

The bridge answers `ready` with `settings` before the user can touch a control, so `COLOR_MAP` and `MONO_MAP` are filled before `setEncodePreset` can run.

- [ ] **Step 6: Use the table in the GUI self-test**

In `tools/uhdr_repack/src/gui/gui_self_test.cpp`, add `#include "encode_presets.h"` and replace the seven `mono_map.options.* = ...;` lines after `EncodeRequest mono_map = req;` with:

```cpp
  apply_encode_preset("mono", &mono_map.options);
```

- [ ] **Step 7: Build and prove it**

Reconfigure (new source), then:

```text
cmake --build tools/uhdr_repack/build --target uhdr_repack
uhdr_repack --hdr-tiff test/hdr-chart/hdr-chart.tif --base test/hdr-chart/sdr-chart.jpg --out tools/uhdr_repack/build/test-out/mono.jpg --preset mono
uhdr_repack --inspect tools/uhdr_repack/build/test-out/mono.jpg
uhdr_repack --hdr-tiff test/hdr-chart/hdr-chart.tif --base test/hdr-chart/sdr-chart.jpg --out tools/uhdr_repack/build/test-out/x.jpg --preset sepia
```

Expected: exit 0; the inspect output reports a 720×960 gain map for a 1440×1920 image; then `bad --preset (use color or mono)` and exit 1.

In a GUI build, run `uhdr_repack --self-test` (expected exit 0), then open the editor with `scripts/run_hdr_chart_edit.ps1` or `.sh`. Clicking Color map shows `JPEG 95/95 · RGB map 1:1 · boost 1×–16× · 3250 nits`, and Mono map shows `JPEG 95/95 · luma map ½ · boost 1×–4.9× · 1000 nits`.

- [ ] **Step 8: Commit**

```text
git add tools/uhdr_repack/include/encode_presets.h tools/uhdr_repack/src/encode_presets.cpp tools/uhdr_repack/CMakeLists.txt tools/uhdr_repack/include/cli.h tools/uhdr_repack/src/cli_encode.cpp tools/uhdr_repack/src/gui/gui_bridge.cpp tools/uhdr_repack/web/app.js tools/uhdr_repack/src/gui/gui_self_test.cpp
git commit -m "Define Color map and Mono map once in C++ for the editor, the CLI, and the tests."
```

---

### Task 5: Delivery checks for the web and Instagram

**Files:**
- Create: `tools/uhdr_repack/include/delivery_check.h`, `tools/uhdr_repack/src/delivery_check.cpp`
- Create: `tools/uhdr_repack/src/gui/check_preview_jpeg.cpp` (GUI builds only)
- Modify: `tools/uhdr_repack/include/gui/sdr_preview_image.h`, `tools/uhdr_repack/src/main.cpp`, `tools/uhdr_repack/src/cli_encode.cpp` (`print_usage`), `tools/uhdr_repack/CMakeLists.txt`

**Interfaces:**
- Consumes: `inspect_ultra_hdr_file` (`InspectReport`: `is_ultra_hdr`, `width`, `height`, `gainmap_width`, `gainmap_height`, `has_mpf`, `has_primary_xmp`, `has_iso_app2`), `read_embedded_icc`, `classify_icc`, `load_binary_file`, `parse_slice_aspect`, `list_slice_output_paths`, `encode_from_paths`, `cli_encode_main`, `load_sdr_preview_image`, `encode_preview_jpeg`.
- Produces:
  - `struct UhdrExpect { int width = 0; int height = 0; int gainmap_scale = 1; size_t max_bytes = 0; std::string single_aspect; bool skip_if_missing = false; };`
  - `constexpr size_t kInstagramMaxBytes = 8u * 1024u * 1024u;`
  - `int verify_uhdr_file(const std::string& path, const UhdrExpect& expect);` returns 0, 1, or 77.
  - `int verify_uhdr_main(int argc, char** argv);`
  - `int check_utf8_path_main(const std::string& hdr, const std::string& sdr, const std::string& out_dir);`
  - `int encode_or_skip_main(int argc, char** argv);`
  - `int check_preview_jpeg_main(const std::string& path);` (GUI only)

- [ ] **Step 1: Show the flag is missing**

```text
uhdr_repack --verify-uhdr tools/uhdr_repack/build/test-out/mono.jpg
```

Expected: exit 1, `unknown argument: --verify-uhdr`, and the usage text.

- [ ] **Step 2: Add the delivery module**

`tools/uhdr_repack/include/delivery_check.h`:

```cpp
#pragma once

#include <cstddef>
#include <string>

namespace uhdr_repack {

/** Instagram's upload limit for photos. */
constexpr size_t kInstagramMaxBytes = 8u * 1024u * 1024u;

struct UhdrExpect {
  int width = 0;             // 0 skips the size check
  int height = 0;
  int gainmap_scale = 1;
  size_t max_bytes = 0;      // 0 skips the file-size limit
  std::string single_aspect; // non-empty: fail when numbered slices exist
  bool skip_if_missing = false;
};

/**
 * Fail unless path is an Ultra HDR JPEG that browsers and Instagram read with its colors:
 * MPF, hdrgm XMP, ISO 21496-1 APP2, a Display P3 / sRGB-curve primary ICC, the expected gain-map
 * scale, xmpRights URLs, and the optional size, byte, and single-slice expectations.
 * @return 0 pass, 1 fail, 77 missing with skip_if_missing.
 */
int verify_uhdr_file(const std::string& path, const UhdrExpect& expect);

int verify_uhdr_main(int argc, char** argv);
int check_utf8_path_main(const std::string& hdr, const std::string& sdr, const std::string& out_dir);

/** Return 77 when --hdr-tiff or --base is missing on disk. Otherwise run the CLI encoder. */
int encode_or_skip_main(int argc, char** argv);

}  // namespace uhdr_repack
```

`tools/uhdr_repack/src/delivery_check.cpp`:

```cpp
#include "delivery_check.h"

#include "cli.h"
#include "encode_engine.h"
#include "icc_profile.h"
#include "jpeg_container.h"
#include "path_io.h"
#include "session.h"
#include "slice_plan.h"
#include "verify.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace uhdr_repack {
namespace fs = std::filesystem;

namespace {

bool contains(const std::vector<uint8_t>& bytes, const char* needle) {
  const std::string hay(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return hay.find(needle) != std::string::npos;
}

bool parse_size(const std::string& spec, int* w, int* h) {
  const size_t x = spec.find('x');
  if (x == std::string::npos) return false;
  *w = std::atoi(spec.c_str());
  *h = std::atoi(spec.c_str() + x + 1);
  return *w >= 2 && *h >= 2 && std::to_string(*w) + "x" + std::to_string(*h) == spec;
}

bool scaled_ok(int image, int map, int scale) {
  return map == image / scale || map == (image + scale - 1) / scale;
}

}  // namespace

int verify_uhdr_file(const std::string& path, const UhdrExpect& expect) {
  if (!fs::exists(path_from_utf8(path))) {
    if (expect.skip_if_missing) {
      std::cout << "SKIP: missing " << path << "\n";
      return 77;
    }
    std::cerr << "missing " << path << "\n";
    return 1;
  }
  std::vector<uint8_t> bytes;
  std::string err;
  if (!load_binary_file(path, &bytes, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  InspectReport report;
  if (!inspect_ultra_hdr_file(path, &report, &err) || !report.is_ultra_hdr) {
    std::cerr << (err.empty() ? "not Ultra HDR: " + path : err) << "\n";
    return 1;
  }
  std::vector<std::string> problems;
  if (!report.has_mpf) problems.push_back("MPF index is missing");
  if (!report.has_primary_xmp) problems.push_back("primary hdrgm XMP is missing");
  if (!report.has_iso_app2) problems.push_back("ISO 21496-1 APP2 block is missing");
  const int s = expect.gainmap_scale < 1 ? 1 : expect.gainmap_scale;
  if (!scaled_ok(report.width, report.gainmap_width, s) || !scaled_ok(report.height, report.gainmap_height, s)) {
    problems.push_back("gain map is " + std::to_string(report.gainmap_width) + "x" +
                       std::to_string(report.gainmap_height) + ", expected image / " + std::to_string(s));
  }
  std::vector<uint8_t> icc;
  if (!read_embedded_icc(path, &icc, &err)) {
    problems.push_back("primary ICC: " + err);
  } else {
    const IccClass c = classify_icc(icc);
    if (c.primaries != IccPrimaries::kDisplayP3 || c.transfer != IccTransfer::kSrgb) {
      problems.push_back("primary ICC is " + icc_class_name(c) + "; browsers need Display P3 / sRGB curve");
    }
  }
  if (!contains(bytes, "https://hdr.karachun.by/") ||
      !contains(bytes, "https://github.com/karachungen/lightroom-plugin-export-hdr")) {
    problems.push_back("xmpRights URLs are missing");
  }
  if (expect.width > 0 && (report.width != expect.width || report.height != expect.height)) {
    problems.push_back("size " + std::to_string(report.width) + "x" + std::to_string(report.height) + " != " +
                       std::to_string(expect.width) + "x" + std::to_string(expect.height));
  }
  if (expect.max_bytes > 0 && bytes.size() > expect.max_bytes) {
    problems.push_back(std::to_string(bytes.size()) + " bytes is over the " + std::to_string(expect.max_bytes) +
                       "-byte limit");
  }
  if (!expect.single_aspect.empty()) {
    SliceAspect aspect = SliceAspect::kNone;
    if (!parse_slice_aspect(expect.single_aspect, &aspect)) {
      problems.push_back("unknown aspect " + expect.single_aspect);
    } else if (!list_slice_output_paths(path, aspect).empty()) {
      problems.push_back("numbered slices were written next to " + path);
    }
  }
  if (!problems.empty()) {
    for (const std::string& p : problems) std::cerr << p << "\n";
    std::cout << "UHDR_VERIFY FAIL " << path << "\n";
    return 1;
  }
  std::cout << "UHDR_VERIFY PASS " << report.width << "x" << report.height << " gain map " << report.gainmap_width
            << "x" << report.gainmap_height << " " << bytes.size() << " bytes\n";
  return 0;
}

int verify_uhdr_main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "--verify-uhdr requires a JPEG path\n";
    return 1;
  }
  UhdrExpect expect;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_value = i + 1 < argc;
    if (a == "--expect-size" && has_value) {
      if (!parse_size(argv[++i], &expect.width, &expect.height)) {
        std::cerr << "--expect-size wants WxH\n";
        return 1;
      }
    } else if (a == "--expect-single" && has_value) {
      expect.single_aspect = argv[++i];
    } else if (a == "--gainmap-scale" && has_value) {
      expect.gainmap_scale = std::atoi(argv[++i]);
    } else if (a == "--max-bytes" && has_value) {
      expect.max_bytes = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (a == "--skip-if-missing") {
      expect.skip_if_missing = true;
    } else {
      std::cerr << "unknown --verify-uhdr argument: " << a << "\n";
      return 1;
    }
  }
  return verify_uhdr_file(argv[2], expect);
}

int check_utf8_path_main(const std::string& hdr, const std::string& sdr, const std::string& out_dir) {
  const char kFolder[] = "\xd1\x82\xd0\xb5\xd1\x81\xd1\x82";  // тест, as UTF-8 bytes whatever the compiler code page
  const fs::path dir = path_from_utf8(out_dir) / path_from_utf8(kFolder);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    std::cerr << "could not create " << dir.u8string() << ": " << ec.message() << "\n";
    return 1;
  }
  EncodeRequest req;
  req.hdr_tiff = hdr;
  req.base_path = sdr;
  req.out_path = (dir / "out_uhdr.jpg").u8string();
  std::string err;
  const int enc = encode_from_paths(req, &err);
  if (enc != 0) {
    std::cerr << err << "\n";
    return enc;
  }
  return verify_uhdr_file(req.out_path, UhdrExpect{});
}

int encode_or_skip_main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) {
    if (i != 1) args.emplace_back(argv[i]);
  }
  for (size_t i = 1; i + 1 < args.size(); ++i) {
    if ((args[i] == "--hdr-tiff" || args[i] == "--base") && !fs::exists(path_from_utf8(args[i + 1]))) {
      std::cout << "SKIP: missing " << args[i + 1] << "\n";
      return 77;
    }
  }
  std::vector<char*> ptrs;
  for (std::string& a : args) ptrs.push_back(a.data());
  ptrs.push_back(nullptr);
  return cli_encode_main(static_cast<int>(args.size()), ptrs.data());
}

}  // namespace uhdr_repack
```

Add `src/delivery_check.cpp` to `_UHDR_CORE_SOURCES`.

- [ ] **Step 3: Add the preview round trip (GUI builds)**

In `tools/uhdr_repack/include/gui/sdr_preview_image.h`:

```cpp
/** Encode a preview JPEG, check the SOI marker, reload it; return 0 or 1. */
int check_preview_jpeg_main(const std::string& path);
```

`tools/uhdr_repack/src/gui/check_preview_jpeg.cpp`, added to `_UHDR_GUI_SOURCES`:

```cpp
#include "gui/sdr_preview_image.h"

#include "path_io.h"

#include <QImage>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace uhdr_repack {
namespace fs = std::filesystem;

int check_preview_jpeg_main(const std::string& path) {
  QImage image;
  std::string error;
  if (!load_sdr_preview_image(path, &image, &error) || image.isNull()) {
    std::cerr << (error.empty() ? "Could not load SDR image" : error) << "\n";
    return 1;
  }
  std::vector<uint8_t> jpeg;
  if (!encode_preview_jpeg(image, 80, &jpeg, &error) || jpeg.size() < 32 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
    std::cerr << (error.empty() ? "Could not encode preview JPEG" : error) << "\n";
    return 1;
  }
  const fs::path out = fs::temp_directory_path() / "uhdr-preview-check.jpg";
  {
    std::ofstream file = open_output_binary(out.u8string());
    file.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    if (!file) {
      std::cerr << "could not write " << out.u8string() << "\n";
      return 1;
    }
  }
  QImage again;
  if (!load_sdr_preview_image(out.u8string(), &again, &error) || again.isNull()) {
    std::cerr << (error.empty() ? "encoded preview JPEG did not reload" : error) << "\n";
    return 1;
  }
  std::cout << "preview jpeg bytes: " << jpeg.size() << "\n";
  return 0;
}

}  // namespace uhdr_repack
```

- [ ] **Step 4: Wire the flags**

In `main.cpp`, include `delivery_check.h`, and before `bool edit_mode = false;` add:

```cpp
  if (std::strcmp(argv[1], "--verify-uhdr") == 0) return verify_uhdr_main(argc, argv);
  if (std::strcmp(argv[1], "--check-utf8-path") == 0) {
    if (argc < 5) {
      std::cerr << "--check-utf8-path requires <hdr-tiff> <sdr> <out-dir>\n";
      print_usage();
      return 1;
    }
    return check_utf8_path_main(argv[2], argv[3], argv[4]);
  }
  if (std::strcmp(argv[1], "--encode-or-skip") == 0) return encode_or_skip_main(argc, argv);
  if (std::strcmp(argv[1], "--check-preview-jpeg") == 0) {
#ifndef UHDR_ENABLE_GUI
    std::cerr << "uhdr_repack was built without GUI support (--check-preview-jpeg unavailable)\n";
    return 1;
#else
    if (argc < 3) {
      std::cerr << "--check-preview-jpeg requires an image path\n";
      return 1;
    }
    return check_preview_jpeg_main(argv[2]);
#endif
  }
```

In `print_usage`, add:

```text
      << "  uhdr_repack --verify-uhdr <jpg> [--expect-size WxH] [--expect-single <aspect>] [--gainmap-scale N] [--max-bytes N] [--skip-if-missing]\n"
      << "  uhdr_repack --check-utf8-path <hdr-tiff> <sdr> <out-dir>\n"
      << "  uhdr_repack --encode-or-skip <encode arguments>\n"
      << "  uhdr_repack --check-preview-jpeg <sdr.jpg>\n"
```

- [ ] **Step 5: Prove the verdicts**

Reconfigure and rebuild.

```text
uhdr_repack --verify-uhdr tools/uhdr_repack/build/test-out/mono.jpg --expect-size 1440x1920 --gainmap-scale 2 --max-bytes 8388608
uhdr_repack --verify-uhdr tools/uhdr_repack/build/test-out/mono.jpg --gainmap-scale 1
uhdr_repack --verify-uhdr test/hdr-chart/sdr-chart.jpg
uhdr_repack --encode-or-skip --hdr-tiff test/no-such.tif --base test/hdr-chart/sdr-chart.jpg --out tools/uhdr_repack/build/test-out/missing.jpg
uhdr_repack --check-utf8-path test/hdr-chart/hdr-chart.tif test/hdr-chart/sdr-chart.jpg tools/uhdr_repack/build/test-out
```

Expected, in order:

1. `UHDR_VERIFY PASS 1440x1920 gain map 720x960 N bytes`, exit 0;
2. `gain map is 720x960, expected image / 1`, `UHDR_VERIFY FAIL`, exit 1;
3. `not Ultra HDR`, exit 1;
4. `SKIP: missing test/no-such.tif`, exit 77;
5. `UHDR_VERIFY PASS 1440x1920 ...`, exit 0, and `tools/uhdr_repack/build/test-out/тест/out_uhdr.jpg` exists.

If check 1 reports a primary ICC problem, that is a real web-color bug. The fix belongs in `uhdr_encode.cpp` (the base must be tagged `UHDR_CG_DISPLAY_P3`), not in the check.

In a GUI build: `uhdr_repack --check-preview-jpeg test/hdr-chart/sdr-chart.jpg` prints `preview jpeg bytes: N` with N ≥ 32 and exits 0.

- [ ] **Step 6: Commit**

```text
git add tools/uhdr_repack/include/delivery_check.h tools/uhdr_repack/src/delivery_check.cpp tools/uhdr_repack/src/gui/check_preview_jpeg.cpp tools/uhdr_repack/include/gui/sdr_preview_image.h tools/uhdr_repack/src/main.cpp tools/uhdr_repack/src/cli_encode.cpp tools/uhdr_repack/CMakeLists.txt
git commit -m "Check Ultra HDR files the way browsers and Instagram read them."
```

---

### Task 6: Grade the chart through the Apply path at SDR, partial, and full headroom

**Files:**
- Modify: `tools/uhdr_repack/src/hdr_chart.cpp`, `tools/uhdr_repack/include/hdr_chart.h`
- Modify: `tools/uhdr_repack/src/main.cpp`, `tools/uhdr_repack/src/cli_encode.cpp` (`print_usage`)

**Interfaces:**
- Consumes: `parse_encode_flag` (Task 4), `verify_uhdr_file`, `UhdrExpect`, `kInstagramMaxBytes` (Task 5), `frame_for`, `inside` (Task 3), `PreviewSession`, `SessionItem`, `ItemEncodeResult`, `encode_session_item` (`session.h`), `resolve_item_export_size` (`slice_plan.h`), libultrahdr `uhdr_dec_get_gainmap_metadata` (fields `max_content_boost[3]`, `min_content_boost[3]`, `offset_sdr[3]`, `offset_hdr[3]`, `hdr_capacity_min`, `hdr_capacity_max`, `use_base_cg`, all linear).
- Produces:
  - `int check_hdr_chart_main(int argc, char** argv);` for `--check-hdr-chart <dir> [--out <jpg>] [encode options]`.
  - `int check_hdr_chart_file_main(int argc, char** argv);` for `--check-hdr-chart-file <jpg-or-tif> [--manifest <json>] [--slice-aspect A] [--crop-offset F]`.
  - A summary line per pass: `HDR_CHART <encoder|lightroom> <boost=B|tiff> <PASS|FAIL> gated=N failed=F outside=M`.

- [ ] **Step 1: Show the check ignores Instagram options today**

```text
uhdr_repack --check-hdr-chart test/hdr-chart --out tools/uhdr_repack/build/test-out/t.jpg --preset mono --slice-aspect 4x5 --out-width 1080
```

Expected: the flags are ignored. `test/hdr-chart/chart-uhdr.jpg` is written at 1440×1920 and `t.jpg` does not exist.

- [ ] **Step 2: Change the public signatures**

In `hdr_chart.h`, replace the two declarations with:

```cpp
/**
 * Encode the chart with encode_session_item (the editor's Apply) using encode options parsed like
 * the CLI (--preset, --slice-aspect, --out-width, ...), verify delivery, then grade at SDR,
 * partial, and full headroom.
 */
int check_hdr_chart_main(int argc, char** argv);

/**
 * Grade an exported Ultra HDR JPEG or HDR TIFF against the manifest. --slice-aspect names the
 * Instagram frame the export used. Only full headroom gates a JPEG; Lightroom renders its own SDR base.
 */
int check_hdr_chart_file_main(int argc, char** argv);
```

In `main.cpp`, replace the `--check-hdr-chart` and `--check-hdr-chart-file` blocks with:

```cpp
  if (std::strcmp(argv[1], "--check-hdr-chart") == 0) return check_hdr_chart_main(argc, argv);
  if (std::strcmp(argv[1], "--check-hdr-chart-file") == 0) return check_hdr_chart_file_main(argc, argv);
```

In `print_usage`, replace the two chart lines with:

```text
      << "  uhdr_repack --check-hdr-chart <dir> [--out <jpg>] [--preset color|mono] [encode options]\n"
      << "  uhdr_repack --check-hdr-chart-file <jpg-or-tif> [--manifest <json>] [--slice-aspect <a>] [--crop-offset <0-1>]\n"
```

- [ ] **Step 3: Make the grader frame-aware**

In `hdr_chart.cpp`, add `#include "cli.h"`, `#include "delivery_check.h"`, and `#include "session.h"`. Before `grade`, add:

```cpp
struct GradeView {
  Rect frame{0, 0, kWidth, kHeight};
  bool lightroom = false;
  std::string pass = "boost=full";
  std::string gain_path;
};
```

Change `gain_rgb` to take the frame. Its signature becomes `bool gain_rgb(const std::string& path, const Rect& frame, const ChartSample& sample, float* r, float* g, float* b, std::string* error)`, and the four `x0`/`y0`/`x1`/`y1` lines become:

```cpp
  const double fx = static_cast<double>(gw) / frame.w;
  const double fy = static_cast<double>(gh) / frame.h;
  const int x0 = std::clamp(static_cast<int>(std::floor((sample.x - frame.x) * fx)), 0, gw - 1);
  const int y0 = std::clamp(static_cast<int>(std::floor((sample.y - frame.y) * fy)), 0, gh - 1);
  const int x1 = std::clamp(static_cast<int>(std::ceil((sample.x + sample.w - frame.x) * fx)), x0 + 1, gw);
  const int y1 = std::clamp(static_cast<int>(std::ceil((sample.y + sample.h - frame.y) * fy)), y0 + 1, gh);
```

Replace `aspect_ok` with:

```cpp
bool frame_aspect_ok(int width, int height, const Rect& frame, std::string* error) {
  const double got = static_cast<double>(width) / height;
  const double want = static_cast<double>(frame.w) / frame.h;
  if (std::fabs(got - want) / want > 0.02) {
    if (error) {
      *error = "image is " + std::to_string(width) + "x" + std::to_string(height) + " but the frame is " +
               std::to_string(frame.w) + "x" + std::to_string(frame.h) +
               ". Pass the --slice-aspect the export used, or export without a crop.";
    }
    return false;
  }
  return true;
}
```

In `grade`, make these edits:

1. The signature becomes `int grade(const std::vector<ChartSample>& samples, const std::vector<float>& rgb, int width, int height, const GradeView& view, float* exposure_out)`.
2. The first lines become:

```cpp
  const float stop_tol = view.lightroom ? kLrStop : kEncStop;
  const float hue_tol = view.lightroom ? kLrHue : kEncHue;
  const Rect& f = view.frame;
  const double sx = static_cast<double>(width) / static_cast<double>(f.w);
  const double sy = static_cast<double>(height) / static_cast<double>(f.h);
  int outside = 0;
```

3. At the top of the sample loop, and with the coordinates shifted by the frame origin:

```cpp
  for (const ChartSample& s : samples) {
    if (!inside(s, f)) {
      ++outside;
      continue;
    }
    const int x0 = static_cast<int>(std::floor((s.x - f.x) * sx));
    const int y0 = static_cast<int>(std::floor((s.y - f.y) * sy));
    const int x1 = static_cast<int>(std::ceil((s.x + s.w - f.x) * sx));
    const int y1 = static_cast<int>(std::ceil((s.y + s.h - f.y) * sy));
```

4. Replace `lightroom` with `view.lightroom` in the `gated` line.
5. In the chromatic block, change `if (!gain_path.empty())` to `if (!view.gain_path.empty())`, change `if (!s.chromatic) continue;` to `if (!s.chromatic || !inside(s, f)) continue;`, and call `gain_rgb(view.gain_path, f, s, &r, &g, &b, &err)`.
6. The last lines become:

```cpp
  const char* mode = view.lightroom ? "lightroom" : "encoder";
  const int gated = npass + nfail;
  std::printf("HDR_CHART %s %s %s gated=%d failed=%d outside=%d\n", mode, view.pass.c_str(),
              nfail ? "FAIL" : "PASS", gated, nfail, outside);
  std::fflush(stdout);
  return nfail ? 1 : 0;
```

- [ ] **Step 4: Model what a display with less headroom shows**

Add these before `check_hdr_chart_main`, inside the anonymous namespace:

```cpp
struct GainMeta {
  float min_boost[3] = {1, 1, 1};
  float max_boost[3] = {16, 16, 16};
  float offset_sdr[3] = {0, 0, 0};
  float offset_hdr[3] = {0, 0, 0};
  float cap_min = 1.0f;
  float cap_max = 16.0f;
  bool use_base_cg = true;
  bool luma = false;
};

int jpeg_components(const uint8_t* data, size_t size) {
  jpeg_decompress_struct cinfo{};
  JpegErr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_fail;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_decompress(&cinfo);
    return 0;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, const_cast<unsigned char*>(data), static_cast<unsigned long>(size));
  const int n = jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK ? cinfo.num_components : 0;
  jpeg_destroy_decompress(&cinfo);
  return n;
}

bool read_gain_meta(const std::string& path, GainMeta* out, std::string* error) {
  std::ifstream input = open_input_binary(path);
  if (!input) {
    if (error) *error = "could not open " + path;
    return false;
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  uhdr_codec_private_t* decoder = uhdr_create_decoder();
  if (!decoder) {
    if (error) *error = "uhdr_create_decoder failed";
    return false;
  }
  uhdr_compressed_image_t compressed{};
  compressed.data = bytes.data();
  compressed.data_sz = bytes.size();
  compressed.capacity = bytes.size();
  compressed.cg = UHDR_CG_UNSPECIFIED;
  compressed.ct = UHDR_CT_UNSPECIFIED;
  compressed.range = UHDR_CR_UNSPECIFIED;
  uhdr_error_info_t st = uhdr_dec_set_image(decoder, &compressed);
  if (st.error_code == UHDR_CODEC_OK) st = uhdr_dec_probe(decoder);
  uhdr_gainmap_metadata_t* m = st.error_code == UHDR_CODEC_OK ? uhdr_dec_get_gainmap_metadata(decoder) : nullptr;
  uhdr_mem_block_t* gain = st.error_code == UHDR_CODEC_OK ? uhdr_dec_get_gainmap_image(decoder) : nullptr;
  if (!m || !gain || !gain->data) {
    uhdr_release_decoder(decoder);
    if (error) *error = "could not read gain-map metadata from " + path;
    return false;
  }
  for (int c = 0; c < 3; ++c) {
    out->min_boost[c] = m->min_content_boost[c];
    out->max_boost[c] = m->max_content_boost[c];
    out->offset_sdr[c] = m->offset_sdr[c];
    out->offset_hdr[c] = m->offset_hdr[c];
  }
  out->cap_min = m->hdr_capacity_min;
  out->cap_max = m->hdr_capacity_max;
  out->use_base_cg = m->use_base_cg != 0;
  out->luma = jpeg_components(static_cast<const uint8_t*>(gain->data), gain->data_sz) == 1;
  uhdr_release_decoder(decoder);
  return true;
}

float gain_weight(const GainMeta& m, float boost) {
  const float lo = std::log2(std::max(1e-6f, m.cap_min));
  const float hi = std::log2(std::max(1e-6f, m.cap_max));
  if (hi <= lo) return boost >= m.cap_max ? 1.0f : 0.0f;
  return std::clamp((std::log2(std::max(1.0f, boost)) - lo) / (hi - lo), 0.0f, 1.0f);
}

bool inside_p3(LinearRgb rec) {
  const LinearRgb p3 = rec2020_to_display_p3(rec);
  const float tol = -1e-4f * std::max(1.0f, max3(rec));
  return p3.r >= tol && p3.g >= tol && p3.b >= tol;
}

float luma_of(LinearRgb c, bool p3) {
  return p3 ? luminance_display_p3(c.r, c.g, c.b) : 0.2627f * c.r + 0.6780f * c.g + 0.0593f * c.b;
}

LinearRgb blended(LinearRgb hdr_rec, const GainMeta& m, float w) {
  const LinearRgb base_p3 = sdr_p3(hdr_rec);
  const LinearRgb s = m.use_base_cg ? base_p3 : display_p3_to_rec2020(base_p3);
  const LinearRgb h = m.use_base_cg ? rec2020_to_display_p3(hdr_rec) : hdr_rec;
  const float sv[3] = {s.r, s.g, s.b};
  const float hv[3] = {h.r, h.g, h.b};
  float o[3];
  for (int c = 0; c < 3; ++c) {
    const float ks = m.offset_sdr[c];
    const float kh = m.offset_hdr[c];
    float g = m.luma ? (luma_of(h, m.use_base_cg) + kh) / (luma_of(s, m.use_base_cg) + ks) : (hv[c] + kh) / (sv[c] + ks);
    g = std::clamp(g, m.min_boost[c], m.max_boost[c]);
    o[c] = (sv[c] + ks) * std::pow(g, w) - kh;
  }
  const LinearRgb out{o[0], o[1], o[2]};
  return m.use_base_cg ? display_p3_to_rec2020(out) : out;
}

std::vector<ChartSample> expected_at_boost(std::vector<ChartSample> samples, const GainMeta& m, float boost, bool gate) {
  const float w = gain_weight(m, boost);
  for (ChartSample& s : samples) {
    if (!gate) {
      s.gate_encoder = false;
      s.gate_lightroom = false;
    }
    if (w <= 0.0f) {
      s.expected = display_p3_to_rec2020(sdr_p3(s.expected));
    } else if (w < 1.0f || m.luma) {
      if (!inside_p3(s.expected)) {
        s.gate_encoder = false;
        s.gate_lightroom = false;
      } else {
        s.expected = blended(s.expected, m, w);
      }
    }
  }
  return samples;
}

std::vector<float> grade_boosts(const GainMeta& m) {
  std::vector<float> out{1.0f};
  if (m.cap_max > 4.0f * 1.01f) out.push_back(4.0f);
  if (m.cap_max > 1.01f) out.push_back(m.cap_max);
  return out;
}

int grade_uhdr_passes(const std::vector<ChartSample>& samples, const std::string& path, const Rect& frame,
                      bool lightroom) {
  GainMeta meta;
  std::string err;
  if (!read_gain_meta(path, &meta, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  std::printf("gain map %s, content boost %.3f-%.3f, capacity %.3f-%.3f, %s color space\n",
              meta.luma ? "luma" : "RGB", meta.min_boost[0], meta.max_boost[0], meta.cap_min, meta.cap_max,
              meta.use_base_cg ? "base" : "alternate");
  int failed = 0;
  for (const float boost : grade_boosts(meta)) {
    const bool full = boost >= meta.cap_max * 0.999f;
    std::vector<float> rgb;
    int w = 0;
    int h = 0;
    if (!decode_uhdr_rec2020(path, boost, &rgb, &w, &h, &err) || !frame_aspect_ok(w, h, frame, &err)) {
      std::cerr << err << "\n";
      return 1;
    }
    char label[32];
    std::snprintf(label, sizeof(label), "boost=%.2f", boost);
    GradeView view;
    view.frame = frame;
    view.lightroom = lightroom;
    view.pass = label;
    if (full && !meta.luma) view.gain_path = path;
    if (full && meta.luma) std::printf("peak2020 chromatic gain check skipped: luma gain map\n");
    failed |= grade(expected_at_boost(samples, meta, boost, !lightroom || full), rgb, w, h, view, nullptr);
  }
  return failed ? 1 : 0;
}
```

The boost-1 pass is the SDR base every browser without HDR shows. Every sample gates against the SDR rule there, including out-of-P3 colors, whose SDR value is the clipped P3 color the base holds. Partial passes gate only in-P3 colors, because the blend space decides how wider colors clip.

- [ ] **Step 5: Encode through `encode_session_item`**

Replace `check_hdr_chart_main` with:

```cpp
int check_hdr_chart_main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "--check-hdr-chart requires a chart directory\n";
    return 1;
  }
  const std::string dir = argv[2];
  const std::string tiff = join_dir(dir, "hdr-chart.tif");
  const std::string jpeg = join_dir(dir, "sdr-chart.jpg");
  const std::string man = join_dir(dir, "chart-manifest.json");
  EncodeRequest req;
  req.out_path = join_dir(dir, "chart-uhdr.jpg");
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) {
      req.out_path = argv[++i];
      continue;
    }
    const int used = parse_encode_flag(argc, argv, &i, &req);
    if (used < 0) return 1;
    if (used == 0) {
      std::cerr << "unknown --check-hdr-chart argument: " << a << "\n";
      return 1;
    }
  }
  if (!req.gainmap_in.empty() || !req.watermark_config.empty() || req.slice_count != 1) {
    std::cerr << "--check-hdr-chart grades one default Apply: no --gainmap-in, no --watermark-config, slice count 1\n";
    return 1;
  }
  if (!fs::exists(path_from_utf8(tiff)) || !fs::exists(path_from_utf8(jpeg)) || !fs::exists(path_from_utf8(man))) {
    std::cerr << "missing chart files; run --write-hdr-chart " << dir << " first\n";
    return 1;
  }
  std::vector<ChartSample> samples;
  int chart_w = 0;
  int chart_h = 0;
  float boost = kBoost;
  std::string err;
  if (!load_manifest(man, &samples, &chart_w, &chart_h, &boost, &err)) {
    std::cerr << err << "\n";
    return 1;
  }

  // The editor's Apply button calls encode_session_item; grading anything else would test a different path.
  PreviewSession session;
  session.default_encode_options = req.options;
  session.default_slice_aspect = req.slice_aspect;
  SessionItem item;
  item.id = "hdr-chart";
  item.sdr = jpeg;
  item.hdr_tiff = tiff;
  item.out = req.out_path;
  item.encode_options = req.options;
  item.has_encode_override = true;
  item.slice_aspect = req.slice_aspect;
  item.has_slice_override = true;
  item.crop_offset = req.crop_offset;
  item.slice_count = 1;
  item.output_width = req.output_width;
  item.output_height = req.output_height;
  item.metadata_patch = req.metadata_patch;
  ItemEncodeResult ir;
  if (encode_session_item(session, item, &ir, &err) != 0) {
    std::cerr << "encode failed: " << (ir.error.empty() ? err : ir.error) << "\n";
    return 1;
  }

  Rect frame;
  unsigned expect_w = 0;
  unsigned expect_h = 0;
  if (!frame_for(chart_w, chart_h, req.slice_aspect, req.crop_offset, &frame, &err) ||
      !resolve_item_export_size(static_cast<unsigned>(chart_w), static_cast<unsigned>(chart_h), req.slice_aspect,
                                req.output_width, req.output_height, &expect_w, &expect_h)) {
    std::cerr << (err.empty() ? "could not resolve the export size" : err) << "\n";
    return 1;
  }
  UhdrExpect expect;
  expect.width = static_cast<int>(expect_w);
  expect.height = static_cast<int>(expect_h);
  expect.gainmap_scale = req.options.gainmap_scale;
  expect.max_bytes = kInstagramMaxBytes;
  expect.single_aspect = req.slice_aspect == SliceAspect::kNone ? "" : slice_aspect_label(req.slice_aspect);
  if (verify_uhdr_file(req.out_path, expect) != 0) return 1;
  return grade_uhdr_passes(samples, req.out_path, frame, false);
}
```

- [ ] **Step 6: Grade exported files in any Instagram frame**

Replace `check_hdr_chart_file_main` with:

```cpp
int check_hdr_chart_file_main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "--check-hdr-chart-file requires an image path\n";
    return 1;
  }
  const std::string path = argv[2];
  std::string manifest;
  SliceAspect aspect = SliceAspect::kNone;
  float crop_offset = 0.5f;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--manifest" && i + 1 < argc) {
      manifest = argv[++i];
    } else if (a == "--slice-aspect" && i + 1 < argc) {
      if (!parse_slice_aspect(argv[++i], &aspect)) {
        std::cerr << "bad --slice-aspect (use none, 1x1, 4x5, 3x4, or 191x100)\n";
        return 1;
      }
    } else if (a == "--crop-offset" && i + 1 < argc) {
      crop_offset = std::strtof(argv[++i], nullptr);
    } else {
      std::cerr << "unknown --check-hdr-chart-file argument: " << a << "\n";
      return 1;
    }
  }
  if (manifest.empty()) manifest = (path_from_utf8(path).parent_path() / "chart-manifest.json").u8string();
  if (!fs::exists(path_from_utf8(manifest))) {
    std::cerr << "chart manifest not found: " << manifest << "\nPass --manifest test/hdr-chart/chart-manifest.json\n";
    return 1;
  }
  std::vector<ChartSample> samples;
  int chart_w = 0;
  int chart_h = 0;
  float boost = kBoost;
  std::string err;
  Rect frame;
  if (!load_manifest(manifest, &samples, &chart_w, &chart_h, &boost, &err) ||
      !frame_for(chart_w, chart_h, aspect, crop_offset, &frame, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  if (!is_tiff_path(path)) return grade_uhdr_passes(samples, path, frame, true);
  std::vector<float> rgb;
  int w = 0;
  int h = 0;
  if (!load_tiff_rec2020(path, &rgb, &w, &h, &err) || !frame_aspect_ok(w, h, frame, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  GradeView view;
  view.frame = frame;
  view.lightroom = true;
  view.pass = "tiff";
  return grade(samples, rgb, w, h, view, nullptr);
}
```

- [ ] **Step 7: Build and run representative cases**

```text
cmake --build tools/uhdr_repack/build --target uhdr_repack
uhdr_repack --write-hdr-chart tools/uhdr_repack/build/test-out/hdr-chart
uhdr_repack --check-hdr-chart tools/uhdr_repack/build/test-out/hdr-chart --out tools/uhdr_repack/build/test-out/c-orig.jpg --preset color
uhdr_repack --check-hdr-chart tools/uhdr_repack/build/test-out/hdr-chart --out tools/uhdr_repack/build/test-out/m-45.jpg --preset mono --slice-aspect 4x5 --out-width 1080
uhdr_repack --check-hdr-chart tools/uhdr_repack/build/test-out/hdr-chart --out tools/uhdr_repack/build/test-out/c-191.jpg --preset color --slice-aspect 191x100 --out-width 1080
uhdr_repack --check-hdr-chart-file tools/uhdr_repack/build/test-out/m-45.jpg --manifest tools/uhdr_repack/build/test-out/hdr-chart/chart-manifest.json --slice-aspect 4x5
uhdr_repack --check-hdr-chart-file tools/uhdr_repack/build/test-out/m-45.jpg --manifest tools/uhdr_repack/build/test-out/hdr-chart/chart-manifest.json
```

Expected:

1. `c-orig.jpg`: `UHDR_VERIFY PASS 1440x1920 gain map 1440x1920`; then `HDR_CHART encoder boost=1.00 PASS`, `boost=4.00 PASS`, and `boost=<capacity> PASS`, each with `outside=0`, and three `... +4 gain RGB ... PASS` lines; exit 0.
2. `m-45.jpg`: `UHDR_VERIFY PASS 1080x1350 gain map 540x675`; `gain map luma`; `boost=1.00 PASS` and `boost=<capacity> PASS` (capacity about 4.92, so there is no separate 4.00 pass); `outside=` equals the specular count; exit 0.
3. `c-191.jpg`: `UHDR_VERIFY PASS 1080x566`; every pass `PASS` with a large `outside=`; exit 0.
4. `HDR_CHART lightroom boost=... PASS` lines, exit 0.
5. `image is 1080x1350 but the frame is 1440x1920 ...`, exit 1.

A gated `FAIL` here is a product defect or a model mismatch. Print the failing row and the `gain map ...` metadata line, and fix the encoder or the model in `blended`. Do not widen a tolerance to pass. If one is truly wrong for both OSes, change it once on its constant with the reason in a comment.

- [ ] **Step 8: Commit**

```text
git add tools/uhdr_repack/src/hdr_chart.cpp tools/uhdr_repack/include/hdr_chart.h tools/uhdr_repack/src/main.cpp tools/uhdr_repack/src/cli_encode.cpp
git commit -m "Grade the chart through Apply in any Instagram frame at SDR, partial, and full headroom."
```

---

### Task 7: Register the matrix with CTest

**Files:**
- Modify: `tools/uhdr_repack/CMakeLists.txt` (append before `install`)

**Interfaces:**
- Consumes: every subcommand from Tasks 2–6.
- Produces: `ctest --test-dir tools/uhdr_repack/build --output-on-failure`, the same tests on both OSes.

- [ ] **Step 1: Confirm CTest has nothing to run**

```text
ctest --test-dir tools/uhdr_repack/build -N
```

Expected: `Total Tests: 0`.

- [ ] **Step 2: Register the tests**

Append before `install(...)`. `CMAKE_CURRENT_SOURCE_DIR` is `tools/uhdr_repack`, so the repo root is two levels up.

```cmake
enable_testing()

get_filename_component(UHDR_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(UHDR_TEST_OUT "${CMAKE_CURRENT_BINARY_DIR}/test-out")
set(UHDR_BIN $<TARGET_FILE:uhdr_repack>)
set(UHDR_CHART_DIR "${UHDR_TEST_OUT}/hdr-chart")
set(UHDR_CHART_HDR "${UHDR_CHART_DIR}/hdr-chart.tif")
set(UHDR_CHART_SDR "${UHDR_CHART_DIR}/sdr-chart.jpg")

add_test(NAME chart.write COMMAND ${UHDR_BIN} --write-hdr-chart "${UHDR_CHART_DIR}")
set_tests_properties(chart.write PROPERTIES LABELS chart TIMEOUT 600 FIXTURES_SETUP chart)

function(uhdr_chart_test name)
  add_test(NAME ${name} COMMAND ${UHDR_BIN} ${ARGN})
  set_tests_properties(${name} PROPERTIES LABELS chart TIMEOUT 600 FIXTURES_REQUIRED chart)
endfunction()

uhdr_chart_test(chart.assets-current --check-hdr-chart-assets "${UHDR_REPO_ROOT}/test/hdr-chart" "${UHDR_CHART_DIR}")
uhdr_chart_test(chart.input.hdr --describe-input "${UHDR_CHART_HDR}" --require-portable)
uhdr_chart_test(chart.input.sdr --describe-input "${UHDR_CHART_SDR}" --require-portable)

foreach(preset color mono)
  uhdr_chart_test(chart.${preset}.original --check-hdr-chart "${UHDR_CHART_DIR}"
    --out "${UHDR_TEST_OUT}/chart-${preset}-original.jpg" --preset ${preset})
  foreach(aspect 1x1 4x5 3x4 191x100)
    uhdr_chart_test(chart.${preset}.ig-${aspect}-1080 --check-hdr-chart "${UHDR_CHART_DIR}"
      --out "${UHDR_TEST_OUT}/chart-${preset}-${aspect}-1080.jpg" --preset ${preset}
      --slice-aspect ${aspect} --out-width 1080)
    uhdr_chart_test(chart.${preset}.ig-${aspect}-native --check-hdr-chart "${UHDR_CHART_DIR}"
      --out "${UHDR_TEST_OUT}/chart-${preset}-${aspect}-native.jpg" --preset ${preset}
      --slice-aspect ${aspect})
  endforeach()
endforeach()

function(uhdr_smoke_pair name verify_args)
  add_test(NAME smoke.${name}.encode COMMAND ${UHDR_BIN} ${ARGN})
  add_test(NAME smoke.${name}.verify COMMAND ${UHDR_BIN} ${verify_args})
  set_tests_properties(smoke.${name}.encode PROPERTIES LABELS smoke TIMEOUT 600
    FIXTURES_REQUIRED chart FIXTURES_SETUP smoke-${name})
  set_tests_properties(smoke.${name}.verify PROPERTIES LABELS smoke TIMEOUT 600
    FIXTURES_REQUIRED smoke-${name})
endfunction()

uhdr_smoke_pair(cli.default
  "--verify-uhdr;${UHDR_TEST_OUT}/cli-default.jpg;--expect-size;1440x1920"
  --hdr-tiff "${UHDR_CHART_HDR}" --base "${UHDR_CHART_SDR}" --out "${UHDR_TEST_OUT}/cli-default.jpg")
uhdr_smoke_pair(cli.slice-1x1
  "--verify-uhdr;${UHDR_TEST_OUT}/cli-1x1.jpg;--expect-size;1440x1440;--expect-single;1x1"
  --hdr-tiff "${UHDR_CHART_HDR}" --base "${UHDR_CHART_SDR}" --out "${UHDR_TEST_OUT}/cli-1x1.jpg"
  --slice-aspect 1x1)
uhdr_smoke_pair(cli.feed-4x5
  "--verify-uhdr;${UHDR_TEST_OUT}/cli-4x5.jpg;--expect-size;1080x1350;--max-bytes;8388608"
  --hdr-tiff "${UHDR_CHART_HDR}" --base "${UHDR_CHART_SDR}" --out "${UHDR_TEST_OUT}/cli-4x5.jpg"
  --slice-aspect 4x5 --out-width 1080)

add_test(NAME smoke.utf8-path COMMAND ${UHDR_BIN} --check-utf8-path
  "${UHDR_CHART_HDR}" "${UHDR_CHART_SDR}" "${UHDR_TEST_OUT}")
set_tests_properties(smoke.utf8-path PROPERTIES LABELS smoke TIMEOUT 600 FIXTURES_REQUIRED chart)

set(UHDR_FEED_HDR "${UHDR_REPO_ROOT}/test/ui/fixtures/DSC02993.tif")
set(UHDR_FEED_SDR "${UHDR_REPO_ROOT}/test/ui/fixtures/DSC02993.jpg")
add_test(NAME smoke.feed.describe COMMAND ${UHDR_BIN} --describe-input "${UHDR_FEED_HDR}"
  --require-portable --skip-if-missing)
add_test(NAME smoke.feed.encode COMMAND ${UHDR_BIN} --encode-or-skip
  --hdr-tiff "${UHDR_FEED_HDR}" --base "${UHDR_FEED_SDR}" --out "${UHDR_TEST_OUT}/feed-1080.jpg"
  --slice-aspect 4x5 --out-width 1080)
add_test(NAME smoke.feed.verify COMMAND ${UHDR_BIN} --verify-uhdr "${UHDR_TEST_OUT}/feed-1080.jpg"
  --expect-size 1080x1350 --max-bytes 8388608 --skip-if-missing)
set_tests_properties(smoke.feed.describe smoke.feed.encode smoke.feed.verify PROPERTIES
  LABELS smoke TIMEOUT 600 SKIP_RETURN_CODE 77)
set_tests_properties(smoke.feed.encode PROPERTIES FIXTURES_SETUP smoke-feed)
set_tests_properties(smoke.feed.verify PROPERTIES FIXTURES_REQUIRED smoke-feed)

if(UHDR_ENABLE_GUI)
  add_test(NAME smoke.probe-sdr COMMAND ${UHDR_BIN} --probe-sdr "${UHDR_CHART_SDR}")
  add_test(NAME smoke.preview-jpeg COMMAND ${UHDR_BIN} --check-preview-jpeg "${UHDR_CHART_SDR}")
  set_tests_properties(smoke.probe-sdr smoke.preview-jpeg PROPERTIES
    LABELS smoke TIMEOUT 600 FIXTURES_REQUIRED chart FAIL_REGULAR_EXPRESSION "Wrong JPEG library version")
endif()
```

`verify_args` is a CMake list, so its items are separated by `;`, and each becomes one argument. `smoke.feed.*` exercise a real Lightroom export when `DSC02993` is present and skip otherwise. Do not copy a fixture in to force a pass.

- [ ] **Step 3: Reconfigure and list**

```text
ctest --test-dir tools/uhdr_repack/build -N
```

Expected names:

- `chart.write`, `chart.assets-current`, `chart.input.hdr`, `chart.input.sdr`;
- `chart.color.original`, `chart.mono.original`;
- `chart.<color|mono>.ig-<1x1|4x5|3x4|191x100>-<1080|native>` (16 tests);
- `smoke.cli.default.encode`/`.verify`, `smoke.cli.slice-1x1.encode`/`.verify`, `smoke.cli.feed-4x5.encode`/`.verify`;
- `smoke.utf8-path`, `smoke.feed.describe`, `smoke.feed.encode`, `smoke.feed.verify`;
- GUI builds also list `smoke.probe-sdr` and `smoke.preview-jpeg`.

That is 36 tests, or 38 in GUI builds.

- [ ] **Step 4: Run everything**

```text
ctest --test-dir tools/uhdr_repack/build --output-on-failure -j 4
```

Expected: every test passes, except that `smoke.feed.*` are reported as skipped when the fixture is absent. Exit 0.

- [ ] **Step 5: Commit**

```text
git add tools/uhdr_repack/CMakeLists.txt
git commit -m "Run the chart matrix and smoke checks through CTest."
```

---

### Task 8: One command in scripts, CI, and the docs

**Files:**
- Delete: `scripts/run_uhdr_test.sh`, `scripts/run_uhdr_test.ps1`, `scripts/check_hdr_chart_snapshot.sh`, `scripts/check_hdr_chart_snapshot.ps1`
- Modify: `scripts/build_plugin.sh` (`cmd_test`), `scripts/build_plugin.ps1` (`Invoke-TestStep`), `scripts/test_uhdr_preview_suite.sh`
- Modify: `.github/workflows/release-plugin.yml`
- Modify: `test/README.md`, `tools/uhdr_repack/README.md`, `CHANGELOG.md`

**Interfaces:**
- Consumes: the CTest suite from Task 7.
- Produces: `ctest --test-dir tools/uhdr_repack/build --output-on-failure` as the only test command.

- [ ] **Step 1: Replace the script bodies**

`cmd_test` in `scripts/build_plugin.sh` becomes:

```bash
cmd_test() {
	ctest --test-dir "$BUILD_DIR" --output-on-failure
}
```

`Invoke-TestStep` in `scripts/build_plugin.ps1` runs one `ctest --test-dir $BuildDir --output-on-failure`, using the `ctest.exe` beside `$CmakeExe`, and throws when `$LASTEXITCODE` is non-zero. There is no `Get-BashExe` branch and no `run_uhdr_test.ps1` call.

In `scripts/test_uhdr_preview_suite.sh`, replace `"$SCRIPT_DIR/run_uhdr_test.sh"` with:

```bash
ctest --test-dir "$REPO_ROOT/tools/uhdr_repack/build" --output-on-failure
```

Delete the four scripts. Leave `run_hdr_chart_edit.*`, `test_macos_shell_quote.sh`, and `test_windows_cmd_quote.ps1` unchanged.

- [ ] **Step 2: One CI step**

In `.github/workflows/release-plugin.yml`, delete both "HDR chart encode snapshot" steps. Insert one step after the Windows cmd quoting step, with no `if:`:

```yaml
      - name: Encoder tests
        shell: bash
        run: ctest --test-dir tools/uhdr_repack/build --output-on-failure
```

- [ ] **Step 3: Document the Lightroom end-to-end check**

`test/README.md`: replace the per-OS run lines with the `ctest` command, and say that `./scripts/build_plugin.sh test` and `.\scripts\build_plugin.ps1 test` run it. Replace the `hdr-raw.tif`/`sdr.jpg` paragraph with the chart description (sections A–G, the stops, the 4:5 guarantee) and this procedure:

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

`tools/uhdr_repack/README.md`: in "Test" and "HDR stop / color chart", show the same `ctest` command for both OSes, drop `run_uhdr_test.sh`, describe the matrix in one paragraph (presets × Instagram frames × SDR/partial/full headroom through Apply), and point at `test/README.md` for the Lightroom check.

`CHANGELOG.md`, under `## Unreleased`:

```markdown
### Fixed

- Display P3 profiles written by the chart tool used the inverse sRGB curve.
- Lightroom's ZIP HDR TIFF and P3 JPEG are read by the same code on macOS and Windows instead of WIC or Core Image.

### Changed

- Encoder tests run with `ctest` on macOS and Windows. The HDR chart is graded through the editor's Apply path for both presets, every Instagram frame, and SDR, partial, and full HDR headroom.
- Color map and Mono map presets are defined once in C++.
```

- [ ] **Step 4: Full run from a clean test-out**

Delete `tools/uhdr_repack/build/test-out`, then:

```text
cmake --build tools/uhdr_repack/build
ctest --test-dir tools/uhdr_repack/build --output-on-failure -j 4
```

Expected: exit 0. Run the same two commands on the other OS (or let CI do it) before merging; the same test names must pass there.

- [ ] **Step 5: Commit**

```text
git add -u scripts .github/workflows/release-plugin.yml test/README.md tools/uhdr_repack/README.md CHANGELOG.md
git commit -m "Run encoder tests with one ctest command on both platforms."
```

Do not `git add` `tools/uhdr_repack/build`.

---

## Self-review against the spec

- **OS-independent web test:** the portable readers (Task 2), portable ICC checks (Task 1), no OS code in the test sources, CTest on both runners (Tasks 7 and 8).
- **Every color and stop, on Instagram and the web:**
  - neutrals −8..+5, hues −4..+4, three gamuts, Rec.2020 peaks, saturation, ColorChecker, and ramps (Task 3);
  - both presets × four Instagram frames × 1080 and native, plus the original frame (Task 7);
  - SDR, partial, and full headroom passes (Task 6);
  - the browser-facing primary ICC, MPF, ISO metadata, and the 8 MB limit (Task 5).
- **Test equals real use:**
  - `encode_session_item` (Task 6);
  - presets from one table sent to the UI (Task 4);
  - the same decoders for Lightroom's ZIP TIFF and P3 JPEG on both OSes (Task 2);
  - a chart TIFF encoded like Lightroom's (Task 3);
  - a manual Lightroom check through the plug-in (Task 8).
- **Known gaps, stated in the spec:** gain-map editing still decodes through the OS; Lightroom's own SDR render is not graded; `DSC02993` tests skip when the fixture is absent.
- **Type consistency:** `FloatTiffInfo` and `SdrJpegInfo` fields are used the same way in Tasks 2, 3, and 6. `UhdrExpect` fields match between Tasks 5 and 6. `GradeView.pass` is a `std::string` everywhere. `parse_encode_flag` returns 1/0/−1 in both callers.
