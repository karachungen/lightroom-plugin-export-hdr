#include "hdr_chart.h"

#include "cli.h"
#include "color_primaries.h"
#include "delivery_check.h"
#include "encode_engine.h"
#include "half_float.h"
#include "icc_profile.h"
#include "path_io.h"
#include "sdr_jpeg.h"
#include "session.h"
#include "slice_plan.h"
#include "tiff_float.h"
#include "tiff_input.h"

#include <ultrahdr_api.h>

#include <nlohmann/json.hpp>

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

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
namespace fs = std::filesystem;

namespace {

constexpr int kWidth = 1440;
constexpr int kHeight = 1920;
constexpr int kStripRows = 16;
constexpr float kBoost = 16.0f;
constexpr float kEncStop = 0.15f;
constexpr float kEncHue = 0.05f;
constexpr float kLrStop = 0.25f;
constexpr float kLrHue = 0.08f;
constexpr float kRamp0 = -6.4f;
constexpr float kRamp1 = 5.4f;

constexpr int kAX = 80, kAY = 62, kAPatch = 88, kAPatchH = 48, kAGap = 6;
constexpr int kBX = 80, kBY = 128, kBPatch = 56, kBGap = 6;
constexpr int kHueStops[7] = {-4, -2, 0, 1, 2, 3, 4};
constexpr int kCX = 544, kCY = 128, kCPatch = 72, kCGap = 6;
constexpr int kEX = 1018, kEY = 128, kEPatch = 52, kEGap = 4;
constexpr int kEGridW = 6 * kEPatch + 5 * kEGap;
constexpr int kEGridH = 4 * kEPatch + 3 * kEGap;
constexpr int kEY2 = kEY + kEGridH + 18;
constexpr int kDX = 544, kDY = 612, kDPatch = 88, kDGap = 6;
constexpr int kPX = 1176, kPY = 640, kPPatch = 80, kPGap = 10;
constexpr int kFX = 96, kFW = 1248;
constexpr int kFRampY = 1180, kFRampH = 110;
constexpr int kFHue0Y = 1310, kFHueH = 78, kFHue3Y = 1408;
constexpr int kSharpY = 1520, kSharpH = 210;
constexpr int kSpecY = 1760, kSpecH = 100;

static_assert(kWidth * 4 == kHeight * 3, "chart must be 3:4");
static_assert(kHeight % kStripRows == 0, "TIFF strips must cover the height");
static_assert(kFW % 24 == 0, "hue sweep must be an integer number of bands");
static_assert(kAX + 14 * kAPatch + 13 * kAGap <= kWidth, "neutral row overflows");
static_assert(kBY + 12 * kBPatch + 11 * kBGap <= kFRampY, "hue grid overlaps the ramp");
static_assert(kAY + kAPatchH <= kBY - 18, "neutral row overlaps the hue labels");
static_assert(kBX + 7 * kBPatch + 6 * kBGap <= kCX - 28, "hue grid overlaps the gamut row labels");
static_assert(kCX + 6 * kCPatch + 5 * kCGap <= kEX, "gamut grid overlaps ColorChecker");
static_assert(kEY2 + kEGridH <= kDY, "ColorChecker overlaps saturation");
static_assert(kDX + 6 * kDPatch + 5 * kDGap <= kPX, "saturation overlaps Rec.2020 peaks");
static_assert(kDY + 6 * kDPatch + 5 * kDGap <= kFRampY, "saturation overlaps the ramp");
static_assert(kPX + kPPatch < kWidth, "Rec.2020 peaks overflow");
static_assert(kPY + 3 * kPPatch + 2 * kPGap <= kFRampY, "peaks overlap the ramp");
static_assert(kFX + kFW <= kWidth, "gradients overflow");
static_assert(kFHue3Y + kFHueH <= kSharpY, "hue sweep overlaps sharpness");
static_assert(kSharpY + kSharpH <= kSpecY, "sharpness overlaps speculars");
static_assert(kSpecY + kSpecH <= kHeight, "speculars overflow");

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct ChartSample {
  std::string id;
  std::string group;
  std::string series;
  int series_index = -1;
  bool monotonic = false;
  bool exposure_ref = false;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int px = 0;
  int py = 0;
  int pw = 0;
  int ph = 0;
  LinearRgb expected{};
  bool gate_encoder = true;
  bool gate_lightroom = true;
  char chromatic = 0;
};

struct JpegErr {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void jpeg_fail(j_common_ptr cinfo) { longjmp(reinterpret_cast<JpegErr*>(cinfo->err)->jump, 1); }

float max3(LinearRgb c) { return std::max(c.r, std::max(c.g, c.b)); }

LinearRgb scale_rgb(LinearRgb c, float s) { return {c.r * s, c.g * s, c.b * s}; }

LinearRgb unit_max(LinearRgb c) {
  const float m = max3(c);
  if (m <= 1e-12f) return {};
  return scale_rgb(c, 1.0f / m);
}

bool rect_inside(Rect r) {
  return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x + r.w <= kWidth && r.y + r.h <= kHeight;
}

Rect sample_of(Rect patch) {
  if (patch.w < 16 || patch.h < 16) return patch;
  const int mx = std::max(4, patch.w / 4);
  const int my = std::max(4, patch.h / 4);
  return {patch.x + mx, patch.y + my, patch.w - 2 * mx, patch.h - 2 * my};
}

LinearRgb p3_edge(int hue_deg) {
  float h = std::fmod(static_cast<float>(hue_deg), 360.0f);
  if (h < 0.0f) h += 360.0f;
  const float sector = h / 60.0f;
  int i = static_cast<int>(sector);
  if (i > 5) i = 5;
  const float f = sector - static_cast<float>(i);
  const float q = 1.0f - f;
  const float t = f;
  LinearRgb p3{};
  switch (i) {
    case 0: p3 = {1, t, 0}; break;
    case 1: p3 = {q, 1, 0}; break;
    case 2: p3 = {0, 1, t}; break;
    case 3: p3 = {0, q, 1}; break;
    case 4: p3 = {t, 0, 1}; break;
    default: p3 = {1, 0, q}; break;
  }
  return unit_max(display_p3_to_rec2020(p3));
}

LinearRgb gamut_unit(int gamut, int index) {
  static const LinearRgb kNative[6] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}};
  LinearRgb rec = kNative[index];
  if (gamut == 0) rec = linear_srgb_to_rec2020(rec);
  else if (gamut == 1) rec = display_p3_to_rec2020(rec);
  return unit_max(rec);
}

LinearRgb at_stop(LinearRgb unit, int stop) { return scale_rgb(unit, std::ldexp(1.0f, stop)); }

const int kCc[24][3] = {
    {115, 82, 68},  {194, 150, 130}, {98, 122, 157}, {87, 108, 67},  {133, 128, 177}, {103, 189, 170},
    {214, 126, 44}, {80, 91, 166},   {193, 90, 99},  {94, 60, 108},  {157, 188, 64},  {224, 163, 46},
    {56, 61, 150},  {70, 148, 73},   {175, 54, 60},  {231, 199, 31}, {187, 86, 149},  {8, 133, 161},
    {243, 243, 242},{200, 200, 200}, {160, 160, 160},{122, 122, 121},{85, 85, 85},    {52, 52, 52},
};

LinearRgb colorchecker(int index, int stop) {
  const LinearRgb lin{srgb_eotf(kCc[index][0] / 255.0f), srgb_eotf(kCc[index][1] / 255.0f),
                      srgb_eotf(kCc[index][2] / 255.0f)};
  return at_stop(linear_srgb_to_rec2020(lin), stop);
}

LinearRgb ramp_pixel(int x) {
  float t = (static_cast<float>(x) + 0.5f - static_cast<float>(kFX)) / static_cast<float>(kFW);
  t = std::clamp(t, 0.0f, 1.0f);
  const float stop = kRamp0 + t * (kRamp1 - kRamp0);
  const float v = std::exp2(stop);
  return {v, v, v};
}

LinearRgb mean_ramp(Rect r) {
  LinearRgb acc{};
  int n = 0;
  for (int y = r.y; y < r.y + r.h; ++y) {
    for (int x = r.x; x < r.x + r.w; ++x) {
      const LinearRgb c = ramp_pixel(x);
      acc.r += c.r;
      acc.g += c.g;
      acc.b += c.b;
      ++n;
    }
  }
  if (n == 0) return {};
  return scale_rgb(acc, 1.0f / static_cast<float>(n));
}

std::string stop_tag(int stop) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%+d", stop);
  return buf;
}

void push_flat(std::vector<ChartSample>* out, ChartSample sample, Rect patch) {
  const Rect s = sample_of(patch);
  sample.x = s.x;
  sample.y = s.y;
  sample.w = s.w;
  sample.h = s.h;
  sample.px = patch.x;
  sample.py = patch.y;
  sample.pw = patch.w;
  sample.ph = patch.h;
  out->push_back(std::move(sample));
}

void push_window(std::vector<ChartSample>* out, ChartSample sample, Rect window) {
  sample.x = window.x;
  sample.y = window.y;
  sample.w = window.w;
  sample.h = window.h;
  out->push_back(std::move(sample));
}

bool build_samples(std::vector<ChartSample>* out, std::string* error) {
  const char* hue_labels[] = {"R", "OR", "Y", "YG", "G", "GC", "C", "CB", "B", "BM", "M", "MR"};
  for (int stop = -8; stop <= 5; ++stop) {
    ChartSample s;
    s.id = "neutral/" + stop_tag(stop);
    s.group = "neutral";
    s.series = "neutral";
    s.series_index = stop + 8;
    s.monotonic = true;
    s.exposure_ref = stop >= 0 && stop <= 2;
    s.expected = {std::ldexp(1.0f, stop), std::ldexp(1.0f, stop), std::ldexp(1.0f, stop)};
    s.gate_encoder = stop < 5;
    s.gate_lightroom = stop < 5;
    const int col = stop + 8;
    push_flat(out, std::move(s), {kAX + col * (kAPatch + kAGap), kAY, kAPatch, kAPatchH});
  }

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

  const char* grow[] = {"R", "Y", "G", "C", "B", "M"};
  const char* gname[] = {"srgb", "p3", "2020"};
  const int gstops[] = {0, 3};
  for (int row = 0; row < 6; ++row) {
    for (int gamut = 0; gamut < 3; ++gamut) {
      for (int si = 0; si < 2; ++si) {
        ChartSample s;
        s.id = std::string("gamut/") + gname[gamut] + "/" + grow[row] + "/" + stop_tag(gstops[si]);
        s.group = "gamut";
        s.expected = at_stop(gamut_unit(gamut, row), gstops[si]);
        const int col = gamut * 2 + si;
        push_flat(out, std::move(s),
                  {kCX + col * (kCPatch + kCGap), kCY + row * (kCPatch + kCGap), kCPatch, kCPatch});
      }
    }
  }

  const char* peak_ch[] = {"R", "G", "B"};
  const int peak_row[] = {0, 2, 4};
  for (int i = 0; i < 3; ++i) {
    ChartSample s;
    s.id = std::string("peak2020/") + peak_ch[i] + "/+4";
    s.group = "peak2020";
    s.expected = at_stop(gamut_unit(2, peak_row[i]), 4);
    s.chromatic = peak_ch[i][0];
    push_flat(out, std::move(s), {kPX, kPY + i * (kPPatch + kPGap), kPPatch, kPPatch});
  }

  const int sat_hue[] = {0, 60, 120, 180, 240, 300};
  const int sat_pct[] = {25, 50, 75, 100};
  for (int row = 0; row < 6; ++row) {
    const LinearRgb unit = p3_edge(sat_hue[row]);
    for (int col = 0; col < 4; ++col) {
      const float sat = static_cast<float>(sat_pct[col]) / 100.0f;
      const LinearRgb mixed{sat * unit.r + (1.0f - sat), sat * unit.g + (1.0f - sat),
                            sat * unit.b + (1.0f - sat)};
      ChartSample s;
      s.id = std::string("sat/") + grow[row] + "/" + std::to_string(sat_pct[col]);
      s.group = "sat";
      s.expected = at_stop(mixed, 2);
      push_flat(out, std::move(s),
                {kDX + col * (kDPatch + kDGap), kDY + row * (kDPatch + kDGap), kDPatch, kDPatch});
    }
  }

  for (int grid = 0; grid < 2; ++grid) {
    const int stop = grid == 0 ? 0 : 2;
    const int y0 = grid == 0 ? kEY : kEY2;
    for (int i = 0; i < 24; ++i) {
      char id[32];
      std::snprintf(id, sizeof(id), "cc/%02d/%s", i + 1, stop_tag(stop).c_str());
      ChartSample s;
      s.id = id;
      s.group = "cc";
      s.expected = colorchecker(i, stop);
      const int col = i % 6;
      const int row = i / 6;
      push_flat(out, std::move(s),
                {kEX + col * (kEPatch + kEGap), y0 + row * (kEPatch + kEGap), kEPatch, kEPatch});
    }
  }

  for (int stop = -6; stop <= 5; ++stop) {
    const float t = (static_cast<float>(stop) - kRamp0) / (kRamp1 - kRamp0);
    const int cx = static_cast<int>(std::lround(static_cast<float>(kFX) + t * static_cast<float>(kFW) - 0.5f));
    Rect window{cx - 16, kFRampY + (kFRampH - 32) / 2, 32, 32};
    ChartSample s;
    s.id = "grad/neutral/" + stop_tag(stop);
    s.group = "grad-neutral";
    s.series = "grad-neutral";
    s.series_index = stop + 6;
    s.monotonic = true;
    s.expected = mean_ramp(window);
    push_window(out, std::move(s), window);
  }

  const int band = kFW / 24;
  for (int pass = 0; pass < 2; ++pass) {
    const int stop = pass == 0 ? 0 : 3;
    const int y = pass == 0 ? kFHue0Y : kFHue3Y;
    const char* group = pass == 0 ? "grad-hue0" : "grad-hue3";
    for (int i = 0; i < 24; ++i) {
      const int hue = i * 15;
      char id[32];
      std::snprintf(id, sizeof(id), "%s/%03d", group, hue);
      ChartSample s;
      s.id = id;
      s.group = group;
      s.expected = at_stop(p3_edge(hue), stop);
      push_flat(out, std::move(s), {kFX + i * band, y, band, kFHueH});
    }
  }

  const int spec_size[] = {4, 6, 8};
  for (int i = 0; i < 3; ++i) {
    ChartSample s;
    s.id = "spec/" + std::to_string(spec_size[i]);
    s.group = "specular";
    s.expected = at_stop({1, 1, 1}, 4);
    s.gate_encoder = false;
    s.gate_lightroom = false;
    const int x = kFX + 48 + i * 80;
    const int y = kSpecY + 42;
    push_flat(out, std::move(s), {x, y, spec_size[i], spec_size[i]});
  }

  for (const ChartSample& s : *out) {
    if (!rect_inside({s.x, s.y, s.w, s.h}) || (s.pw > 0 && !rect_inside({s.px, s.py, s.pw, s.ph}))) {
      if (error) *error = "chart sample is outside the canvas: " + s.id;
      return false;
    }
  }
  return true;
}

void put_hdr(std::vector<float>& rgb, int x, int y, LinearRgb c) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
  const size_t i = (static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 3u;
  rgb[i] = c.r;
  rgb[i + 1] = c.g;
  rgb[i + 2] = c.b;
}

LinearRgb sdr_p3(LinearRgb rec) {
  rec.r = std::max(0.0f, rec.r);
  rec.g = std::max(0.0f, rec.g);
  rec.b = std::max(0.0f, rec.b);
  // Clip after the gamut conversion. Scaling the Rec.2020 vector down first shrinks the small
  // positive P3 channel of an out-of-gamut primary (Rec.2020 red's P3 blue) while the HDR value
  // does not, so that channel's gain hits the boost ceiling and the gain map is no longer a primary.
  LinearRgb p3 = rec2020_to_display_p3(rec);
  p3.r = std::clamp(p3.r, 0.0f, 1.0f);
  p3.g = std::clamp(p3.g, 0.0f, 1.0f);
  p3.b = std::clamp(p3.b, 0.0f, 1.0f);
  return p3;
}

void put_sdr(std::vector<uint8_t>& rgba, int x, int y, LinearRgb rec) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
  const LinearRgb p3 = sdr_p3(rec);
  const size_t i = (static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 4u;
  rgba[i] = static_cast<uint8_t>(std::lround(srgb_oetf(p3.r) * 255.0f));
  rgba[i + 1] = static_cast<uint8_t>(std::lround(srgb_oetf(p3.g) * 255.0f));
  rgba[i + 2] = static_cast<uint8_t>(std::lround(srgb_oetf(p3.b) * 255.0f));
  rgba[i + 3] = 255;
}

void fill_rect(std::vector<float>& hdr, std::vector<uint8_t>& sdr, int x, int y, int w, int h, LinearRgb c) {
  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) {
      put_hdr(hdr, x + xx, y + yy, c);
      put_sdr(sdr, x + xx, y + yy, c);
    }
  }
}

void fill_patch(std::vector<float>& hdr, std::vector<uint8_t>& sdr, int x, int y, int w, int h, LinearRgb c) {
  if (w < 16 || h < 16) {
    fill_rect(hdr, sdr, x, y, w, h, c);
    return;
  }
  const LinearRgb frame{0.22f, 0.22f, 0.22f};
  fill_rect(hdr, sdr, x, y, w, h, frame);
  fill_rect(hdr, sdr, x + 2, y + 2, w - 4, h - 4, c);
}

const uint8_t* glyph(char ch) {
  static const uint8_t kEmpty[5] = {0, 0, 0, 0, 0};
  static const uint8_t kPlus[5] = {0x08, 0x08, 0x3E, 0x08, 0x08};
  static const uint8_t kMinus[5] = {0x00, 0x00, 0x3E, 0x00, 0x00};
  static const uint8_t k0[5] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
  static const uint8_t k1[5] = {0x00, 0x42, 0x7F, 0x40, 0x00};
  static const uint8_t k2[5] = {0x42, 0x61, 0x51, 0x49, 0x46};
  static const uint8_t k3[5] = {0x21, 0x41, 0x45, 0x4B, 0x31};
  static const uint8_t k4[5] = {0x18, 0x14, 0x12, 0x7F, 0x10};
  static const uint8_t k5[5] = {0x27, 0x45, 0x45, 0x45, 0x39};
  static const uint8_t k6[5] = {0x3C, 0x4A, 0x49, 0x49, 0x30};
  static const uint8_t k7[5] = {0x01, 0x71, 0x09, 0x05, 0x03};
  static const uint8_t k8[5] = {0x36, 0x49, 0x49, 0x49, 0x36};
  static const uint8_t k9[5] = {0x06, 0x49, 0x49, 0x29, 0x1E};
  static const uint8_t kA[5] = {0x7E, 0x09, 0x09, 0x09, 0x7E};
  static const uint8_t kB[5] = {0x7F, 0x49, 0x49, 0x49, 0x36};
  static const uint8_t kC[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};
  static const uint8_t kD[5] = {0x7F, 0x41, 0x41, 0x22, 0x1C};
  static const uint8_t kE[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};
  static const uint8_t kF[5] = {0x7F, 0x09, 0x09, 0x09, 0x01};
  static const uint8_t kG[5] = {0x3E, 0x41, 0x49, 0x49, 0x7A};
  static const uint8_t kH[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
  static const uint8_t kI[5] = {0x00, 0x41, 0x7F, 0x41, 0x00};
  static const uint8_t kJ[5] = {0x20, 0x40, 0x41, 0x3F, 0x01};
  static const uint8_t kK[5] = {0x7F, 0x08, 0x14, 0x22, 0x41};
  static const uint8_t kL[5] = {0x7F, 0x40, 0x40, 0x40, 0x40};
  static const uint8_t kM[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
  static const uint8_t kN[5] = {0x7F, 0x04, 0x08, 0x10, 0x7F};
  static const uint8_t kO[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
  static const uint8_t kP[5] = {0x7F, 0x09, 0x09, 0x09, 0x06};
  static const uint8_t kQ[5] = {0x3E, 0x41, 0x51, 0x21, 0x5E};
  static const uint8_t kR[5] = {0x7F, 0x09, 0x19, 0x29, 0x46};
  static const uint8_t kS[5] = {0x26, 0x49, 0x49, 0x49, 0x32};
  static const uint8_t kT[5] = {0x01, 0x01, 0x7F, 0x01, 0x01};
  static const uint8_t kU[5] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
  static const uint8_t kV[5] = {0x1F, 0x20, 0x40, 0x20, 0x1F};
  static const uint8_t kW[5] = {0x7F, 0x20, 0x18, 0x20, 0x7F};
  static const uint8_t kX[5] = {0x63, 0x14, 0x08, 0x14, 0x63};
  static const uint8_t kY[5] = {0x03, 0x04, 0x78, 0x04, 0x03};
  static const uint8_t kZ[5] = {0x61, 0x51, 0x49, 0x45, 0x43};
  if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  switch (ch) {
    case '+': return kPlus;
    case '-': return kMinus;
    case '0': return k0;
    case '1': return k1;
    case '2': return k2;
    case '3': return k3;
    case '4': return k4;
    case '5': return k5;
    case '6': return k6;
    case '7': return k7;
    case '8': return k8;
    case '9': return k9;
    case 'A': return kA;
    case 'B': return kB;
    case 'C': return kC;
    case 'D': return kD;
    case 'E': return kE;
    case 'F': return kF;
    case 'G': return kG;
    case 'H': return kH;
    case 'I': return kI;
    case 'J': return kJ;
    case 'K': return kK;
    case 'L': return kL;
    case 'M': return kM;
    case 'N': return kN;
    case 'O': return kO;
    case 'P': return kP;
    case 'Q': return kQ;
    case 'R': return kR;
    case 'S': return kS;
    case 'T': return kT;
    case 'U': return kU;
    case 'V': return kV;
    case 'W': return kW;
    case 'X': return kX;
    case 'Y': return kY;
    case 'Z': return kZ;
    default: return kEmpty;
  }
}

int text_px(const char* text) {
  int w = 0;
  for (const char* p = text; *p; ++p) w += (*p == ' ') ? 8 : 12;
  return w;
}

void draw_text(std::vector<float>& hdr, std::vector<uint8_t>& sdr, int x, int y, const char* text) {
  const LinearRgb ink{0.72f, 0.72f, 0.72f};
  int cx = x;
  for (const char* p = text; *p; ++p) {
    if (*p == ' ') {
      cx += 8;
      continue;
    }
    const uint8_t* g = glyph(*p);
    for (int col = 0; col < 5; ++col) {
      for (int row = 0; row < 7; ++row) {
        if ((g[col] >> row) & 1u) {
          for (int oy = 0; oy < 2; ++oy) {
            for (int ox = 0; ox < 2; ++ox) {
              put_hdr(hdr, cx + col * 2 + ox, y + row * 2 + oy, ink);
              put_sdr(sdr, cx + col * 2 + ox, y + row * 2 + oy, ink);
            }
          }
        }
      }
    }
    cx += 12;
  }
}

void draw_text_centered(std::vector<float>& hdr, std::vector<uint8_t>& sdr, int x, int y, int w, const char* text) {
  draw_text(hdr, sdr, x + std::max(0, (w - text_px(text)) / 2), y, text);
}

void draw_line_pairs(std::vector<float>& hdr, std::vector<uint8_t>& sdr, int x, int y, int w, int h, int period,
                     LinearRgb hi) {
  const LinearRgb black{};
  const int half = std::max(1, period);
  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) {
      const bool on = ((xx / half) % 2) == 0;
      put_hdr(hdr, x + xx, y + yy, on ? hi : black);
      put_sdr(sdr, x + xx, y + yy, on ? hi : black);
    }
  }
}

void draw_slant(std::vector<float>& hdr, std::vector<uint8_t>& sdr, int x, int y, int w, int h, LinearRgb hi) {
  const LinearRgb black{};
  for (int yy = 0; yy < h; ++yy) {
    const int edge = std::clamp(4 + yy / 4, 1, w - 2);
    for (int xx = 0; xx < w; ++xx) {
      const bool on = xx >= edge;
      put_hdr(hdr, x + xx, y + yy, on ? hi : black);
      put_sdr(sdr, x + xx, y + yy, on ? hi : black);
    }
  }
}

void draw_labels(std::vector<float>& hdr, std::vector<uint8_t>& sdr) {
  draw_text(hdr, sdr, 8, kAY - 18, "STOPS");
  for (int stop = -8; stop <= 5; ++stop) {
    const int col = stop + 8;
    const int x = kAX + col * (kAPatch + kAGap);
    draw_text_centered(hdr, sdr, x, kAY - 18, kAPatch, stop_tag(stop).c_str());
  }
  draw_text(hdr, sdr, 8, kBY - 16, "HUE");
  for (int col = 0; col < 7; ++col) {
    draw_text_centered(hdr, sdr, kBX + col * (kBPatch + kBGap), kBY - 16, kBPatch, stop_tag(kHueStops[col]).c_str());
  }
  const char* hue_labels[] = {"R", "OR", "Y", "YG", "G", "GC", "C", "CB", "B", "BM", "M", "MR"};
  for (int row = 0; row < 12; ++row) {
    const int y = kBY + row * (kBPatch + kBGap) + kBPatch / 2 - 7;
    draw_text(hdr, sdr, 8, y, hue_labels[row]);
  }
  draw_text(hdr, sdr, 8, kCY + 4, "GAMUT");
  const char* glabels[] = {"S+0", "S+3", "P+0", "P+3", "2+0", "2+3"};
  for (int col = 0; col < 6; ++col) {
    draw_text_centered(hdr, sdr, kCX + col * (kCPatch + kCGap), kCY - 16, kCPatch, glabels[col]);
  }
  const char* grow[] = {"R", "Y", "G", "C", "B", "M"};
  for (int row = 0; row < 6; ++row) {
    const int y = kCY + row * (kCPatch + kCGap) + kCPatch / 2 - 7;
    draw_text(hdr, sdr, kCX - 28, y, grow[row]);
  }
  draw_text(hdr, sdr, kEX, kEY - 16, "+0");
  draw_text(hdr, sdr, kEX, kEY2 - 16, "+2");
  draw_text(hdr, sdr, 8, kDY + 4, "SAT");
  const char* slabs[] = {"25", "50", "75", "100"};
  for (int col = 0; col < 4; ++col) {
    draw_text_centered(hdr, sdr, kDX + col * (kDPatch + kDGap), kDY - 16, kDPatch, slabs[col]);
  }
  for (int row = 0; row < 6; ++row) {
    const int y = kDY + row * (kDPatch + kDGap) + kDPatch / 2 - 7;
    draw_text(hdr, sdr, kDX - 28, y, grow[row]);
  }
  const char* peaks[] = {"R", "G", "B"};
  for (int i = 0; i < 3; ++i) {
    const int y = kPY + i * (kPPatch + kPGap) + kPPatch / 2 - 7;
    draw_text(hdr, sdr, kPX + kPPatch + 8, y, peaks[i]);
  }
  draw_text(hdr, sdr, kPX, kPY - 16, "+4");
  draw_text(hdr, sdr, 8, kFRampY + 8, "RAMP");
  draw_text(hdr, sdr, 8, kFHue0Y + 8, "H+0");
  draw_text(hdr, sdr, 8, kFHue3Y + 8, "H+3");
  const int panel = kFW / 3;
  const char* sharp[] = {"+0", "+2", "+4"};
  for (int p = 0; p < 3; ++p) draw_text(hdr, sdr, kFX + p * panel + 4, kSharpY, sharp[p]);
  const char* spec[] = {"4", "6", "8"};
  for (int i = 0; i < 3; ++i) draw_text(hdr, sdr, kFX + 40 + i * 80, kSpecY + 8, spec[i]);
}

void render_chart(std::vector<float>& hdr, std::vector<uint8_t>& sdr, const std::vector<ChartSample>& samples) {
  hdr.assign(static_cast<size_t>(kWidth) * kHeight * 3u, 0.0f);
  sdr.assign(static_cast<size_t>(kWidth) * kHeight * 4u, 0);
  const LinearRgb bg{0.03f, 0.03f, 0.03f};
  fill_rect(hdr, sdr, 0, 0, kWidth, kHeight, bg);
  fill_rect(hdr, sdr, kFX, kSpecY, 360, kSpecH, LinearRgb{0.01f, 0.01f, 0.01f});
  for (const ChartSample& s : samples) {
    if (s.pw > 0) fill_patch(hdr, sdr, s.px, s.py, s.pw, s.ph, s.expected);
  }
  for (int y = 0; y < kFRampH; ++y) {
    for (int x = 0; x < kFW; ++x) {
      const LinearRgb c = ramp_pixel(kFX + x);
      put_hdr(hdr, kFX + x, kFRampY + y, c);
      put_sdr(sdr, kFX + x, kFRampY + y, c);
    }
  }
  const int panel = kFW / 3;
  const int stops[] = {0, 2, 4};
  for (int p = 0; p < 3; ++p) {
    const int px = kFX + p * panel;
    const LinearRgb hi = at_stop({1, 1, 1}, stops[p]);
    const int band_w = (panel - 16) / 4;
    const int by = kSharpY + 20;
    const int bh = kSharpH - 28;
    draw_line_pairs(hdr, sdr, px + 2, by, band_w, bh, 1, hi);
    draw_line_pairs(hdr, sdr, px + 4 + band_w, by, band_w, bh, 2, hi);
    draw_line_pairs(hdr, sdr, px + 6 + 2 * band_w, by, band_w, bh, 4, hi);
    draw_slant(hdr, sdr, px + 8 + 3 * band_w, by, band_w, bh, hi);
  }
  draw_labels(hdr, sdr);
}

nlohmann::json manifest_json(const std::vector<ChartSample>& samples) {
  nlohmann::json rows = nlohmann::json::array();
  for (const ChartSample& s : samples) {
    rows.push_back({{"id", s.id},
                    {"group", s.group},
                    {"series", s.series},
                    {"series_index", s.series_index},
                    {"monotonic", s.monotonic},
                    {"exposure_ref", s.exposure_ref},
                    {"rect", {s.x, s.y, s.w, s.h}},
                    {"expected_rec2020", {s.expected.r, s.expected.g, s.expected.b}},
                    {"gate", {{"encoder", s.gate_encoder ? "pass" : "report"},
                              {"lightroom", s.gate_lightroom ? "pass" : "report"}}},
                    {"chromatic", s.chromatic ? std::string(1, s.chromatic) : ""}});
  }
  return {{"version", 2},
          {"width", kWidth},
          {"height", kHeight},
          {"max_content_boost", kBoost},
          {"samples", rows}};
}

bool load_manifest(const std::string& path, std::vector<ChartSample>* samples, int* width, int* height,
                   float* boost, std::string* error) {
  std::ifstream in = open_input_binary(path);
  if (!in) {
    if (error) *error = "could not open " + path;
    return false;
  }
  nlohmann::json j;
  try {
    in >> j;
  } catch (const std::exception& ex) {
    if (error) *error = std::string("chart manifest is not JSON: ") + ex.what();
    return false;
  }
  if (j.value("version", 0) != 2) {
    if (error) *error = "chart manifest version is not 2; regenerate with --write-hdr-chart";
    return false;
  }
  *width = j.value("width", 0);
  *height = j.value("height", 0);
  *boost = j.value("max_content_boost", kBoost);
  if (*width < 2 || *height < 2 || !j.contains("samples")) {
    if (error) *error = "chart manifest is missing samples";
    return false;
  }
  samples->clear();
  for (const auto& row : j["samples"]) {
    ChartSample s;
    s.id = row.value("id", "");
    s.group = row.value("group", "");
    s.series = row.value("series", "");
    s.series_index = row.value("series_index", -1);
    s.monotonic = row.value("monotonic", false);
    s.exposure_ref = row.value("exposure_ref", false);
    const auto& rect = row.at("rect");
    s.x = rect.at(0).get<int>();
    s.y = rect.at(1).get<int>();
    s.w = rect.at(2).get<int>();
    s.h = rect.at(3).get<int>();
    const auto& exp = row.at("expected_rec2020");
    s.expected = {exp.at(0).get<float>(), exp.at(1).get<float>(), exp.at(2).get<float>()};
    const auto& gate = row.at("gate");
    s.gate_encoder = gate.value("encoder", "pass") == "pass";
    s.gate_lightroom = gate.value("lightroom", "pass") == "pass";
    const std::string chromatic = row.value("chromatic", "");
    s.chromatic = chromatic.empty() ? 0 : chromatic[0];
    if (s.id.empty() || s.w < 1 || s.h < 1) {
      if (error) *error = "chart manifest has an empty sample";
      return false;
    }
    samples->push_back(std::move(s));
  }
  return !samples->empty();
}

void put_u16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = static_cast<uint8_t>(v);
  b[at + 1] = static_cast<uint8_t>(v >> 8);
}

void put_u32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  b[at] = static_cast<uint8_t>(v);
  b[at + 1] = static_cast<uint8_t>(v >> 8);
  b[at + 2] = static_cast<uint8_t>(v >> 16);
  b[at + 3] = static_cast<uint8_t>(v >> 24);
}

void apply_float_predictor(uint8_t* row, size_t values, uint32_t stride) {
  const size_t bytes = values * 4u;
  std::vector<uint8_t> planes(bytes);
  for (size_t v = 0; v < values; ++v) {
    for (size_t b = 0; b < 4; ++b) planes[(3 - b) * values + v] = row[v * 4u + b];
  }
  for (size_t i = bytes; i-- > stride;) planes[i] = static_cast<uint8_t>(planes[i] - planes[i - stride]);
  std::memcpy(row, planes.data(), bytes);
}

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

bool encode_p3_jpeg(const uint8_t* rgba, const std::vector<uint8_t>& icc, std::vector<uint8_t>* jpeg,
                    std::string* error) {
  jpeg_compress_struct cinfo{};
  JpegErr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_fail;
  unsigned char* outbuf = nullptr;
  unsigned long outsize = 0;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_compress(&cinfo);
    if (outbuf) free(outbuf);
    if (error) *error = "libjpeg failed to compress the chart JPEG";
    return false;
  }
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &outbuf, &outsize);
  cinfo.image_width = static_cast<JDIMENSION>(kWidth);
  cinfo.image_height = static_cast<JDIMENSION>(kHeight);
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 95, TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  std::vector<uint8_t> marker;
  const char hdr[] = "ICC_PROFILE";
  marker.insert(marker.end(), hdr, hdr + 12);
  marker.push_back(1);
  marker.push_back(1);
  marker.insert(marker.end(), icc.begin(), icc.end());
  jpeg_write_marker(&cinfo, JPEG_APP0 + 2, marker.data(), static_cast<unsigned int>(marker.size()));
  std::vector<uint8_t> row(static_cast<size_t>(kWidth) * 3u);
  while (cinfo.next_scanline < cinfo.image_height) {
    const uint8_t* src = rgba + static_cast<size_t>(cinfo.next_scanline) * kWidth * 4u;
    for (int x = 0; x < kWidth; ++x) {
      row[static_cast<size_t>(x) * 3u] = src[static_cast<size_t>(x) * 4u];
      row[static_cast<size_t>(x) * 3u + 1] = src[static_cast<size_t>(x) * 4u + 1];
      row[static_cast<size_t>(x) * 3u + 2] = src[static_cast<size_t>(x) * 4u + 2];
    }
    JSAMPROW rows[1] = {row.data()};
    jpeg_write_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  if (!outbuf || outsize == 0) {
    if (error) *error = "libjpeg produced an empty chart JPEG";
    return false;
  }
  jpeg->assign(outbuf, outbuf + outsize);
  free(outbuf);
  return true;
}

bool write_bytes(const std::string& path, const std::vector<uint8_t>& bytes, std::string* error) {
  std::ofstream out = open_output_binary(path);
  if (!out) {
    if (error) *error = "could not write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

LinearRgb sample_mean(const std::vector<float>& rgb, int width, int height, int x0, int y0, int rw, int rh) {
  const int x1 = std::min(width, x0 + rw);
  const int y1 = std::min(height, y0 + rh);
  LinearRgb acc{};
  int n = 0;
  for (int y = std::max(0, y0); y < y1; ++y) {
    for (int x = std::max(0, x0); x < x1; ++x) {
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3u;
      acc.r += rgb[i];
      acc.g += rgb[i + 1];
      acc.b += rgb[i + 2];
      ++n;
    }
  }
  if (n == 0) return {};
  return scale_rgb(acc, 1.0f / static_cast<float>(n));
}

bool samples_match(const std::vector<float>& rgb, int width, int height, const std::vector<ChartSample>& samples,
                   const char* reader, std::string* error) {
  if (width != kWidth || height != kHeight) {
    if (error) {
      *error = std::string(reader) + " size " + std::to_string(width) + "x" + std::to_string(height) +
               " != chart";
    }
    return false;
  }
  for (const ChartSample& s : samples) {
    const LinearRgb got = sample_mean(rgb, width, height, s.x, s.y, s.w, s.h);
    for (int c = 0; c < 3; ++c) {
      const float exp = c == 0 ? s.expected.r : c == 1 ? s.expected.g : s.expected.b;
      const float g = c == 0 ? got.r : c == 1 ? got.g : got.b;
      const float limit = 0.01f * std::max(1.0f, std::fabs(exp)) + 0.002f;
      if (std::fabs(g - exp) > limit) {
        if (error) {
          *error = std::string(reader) + " mismatch at " + s.id + " expected (" +
                   std::to_string(s.expected.r) + ", " + std::to_string(s.expected.g) + ", " +
                   std::to_string(s.expected.b) + ") got (" + std::to_string(got.r) + ", " +
                   std::to_string(got.g) + ", " + std::to_string(got.b) + ")";
        }
        return false;
      }
    }
  }
  return true;
}

bool half_image_to_rgb(const RawImageHolder& hdr, std::vector<float>* rgb, int* width, int* height,
                       std::string* error) {
  const uhdr_raw_image_t& image = hdr.ref();
  if (!image.planes[UHDR_PLANE_PACKED]) {
    if (error) *error = "fast TIFF reader returned no pixels";
    return false;
  }
  *width = static_cast<int>(image.w);
  *height = static_cast<int>(image.h);
  rgb->assign(static_cast<size_t>(image.w) * image.h * 3u, 0.0f);
  const auto* src = static_cast<const uint16_t*>(image.planes[UHDR_PLANE_PACKED]);
  for (unsigned y = 0; y < image.h; ++y) {
    for (unsigned x = 0; x < image.w; ++x) {
      const size_t si = (static_cast<size_t>(y) * image.stride[UHDR_PLANE_PACKED] + x) * 4u;
      const size_t di = (static_cast<size_t>(y) * image.w + x) * 3u;
      (*rgb)[di] = half_to_float(src[si]);
      (*rgb)[di + 1] = half_to_float(src[si + 1]);
      (*rgb)[di + 2] = half_to_float(src[si + 2]);
    }
  }
  return true;
}

std::string join_dir(const std::string& dir, const char* name) {
  return (path_from_utf8(dir) / name).u8string();
}

LinearRgb to_rec2020(LinearRgb c, int cg) {
  if (cg == 2) return c;
  if (cg == 1) return display_p3_to_rec2020(c);
  return linear_srgb_to_rec2020(c);
}

bool decode_uhdr_rec2020(const std::string& path, float display_boost, std::vector<float>* rgb, int* width,
                         int* height, std::string* error) {
  std::ifstream input = open_input_binary(path);
  if (!input) {
    if (error) *error = "could not open " + path;
    return false;
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    if (error) *error = "empty Ultra HDR file";
    return false;
  }
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
  auto fail = [&](const char* op, const uhdr_error_info_t& st) {
    uhdr_release_decoder(decoder);
    if (error) {
      *error = op;
      if (st.has_detail) {
        *error += ": ";
        *error += st.detail;
      }
    }
    return false;
  };
  uhdr_error_info_t st = uhdr_dec_set_image(decoder, &compressed);
  if (st.error_code != UHDR_CODEC_OK) return fail("uhdr_dec_set_image", st);
  st = uhdr_dec_set_out_img_format(decoder, UHDR_IMG_FMT_64bppRGBAHalfFloat);
  if (st.error_code != UHDR_CODEC_OK) return fail("uhdr_dec_set_out_img_format", st);
  st = uhdr_dec_set_out_color_transfer(decoder, UHDR_CT_LINEAR);
  if (st.error_code != UHDR_CODEC_OK) return fail("uhdr_dec_set_out_color_transfer", st);
  st = uhdr_dec_set_out_max_display_boost(decoder, std::max(1.0f, display_boost));
  if (st.error_code != UHDR_CODEC_OK) return fail("uhdr_dec_set_out_max_display_boost", st);
  st = uhdr_decode(decoder);
  if (st.error_code != UHDR_CODEC_OK) return fail("uhdr_decode", st);
  uhdr_raw_image_t* image = uhdr_get_decoded_image(decoder);
  if (!image || !image->planes[UHDR_PLANE_PACKED]) {
    uhdr_release_decoder(decoder);
    if (error) *error = "decoder returned no image";
    return false;
  }
  *width = static_cast<int>(image->w);
  *height = static_cast<int>(image->h);
  const int cg = static_cast<int>(image->cg);
  rgb->assign(static_cast<size_t>(image->w) * image->h * 3u, 0.0f);
  const auto* src = static_cast<const uint16_t*>(image->planes[UHDR_PLANE_PACKED]);
  for (unsigned y = 0; y < image->h; ++y) {
    for (unsigned x = 0; x < image->w; ++x) {
      const size_t si = (static_cast<size_t>(y) * image->stride[UHDR_PLANE_PACKED] + x) * 4u;
      const size_t di = (static_cast<size_t>(y) * image->w + x) * 3u;
      const LinearRgb rec = to_rec2020({half_to_float(src[si]), half_to_float(src[si + 1]), half_to_float(src[si + 2])}, cg);
      (*rgb)[di] = rec.r;
      (*rgb)[di + 1] = rec.g;
      (*rgb)[di + 2] = rec.b;
    }
  }
  uhdr_release_decoder(decoder);
  return true;
}

bool load_tiff_rec2020(const std::string& path, std::vector<float>* rgb, int* width, int* height,
                       std::string* error) {
  RawImageHolder hdr;
  if (!load_hdr_tiff_raw(path, &hdr, error)) return false;
  return half_image_to_rgb(hdr, rgb, width, height, error);
}

bool decode_jpeg_rgb(const uint8_t* data, unsigned long size, std::vector<uint8_t>* rgb, int* width, int* height,
                     std::string* error) {
  jpeg_decompress_struct cinfo{};
  JpegErr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_fail;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_decompress(&cinfo);
    if (error) *error = "libjpeg failed to decode the gain map";
    return false;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, const_cast<unsigned char*>(data), size);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    if (error) *error = "gain map is not a JPEG";
    return false;
  }
  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);
  *width = static_cast<int>(cinfo.output_width);
  *height = static_cast<int>(cinfo.output_height);
  rgb->assign(static_cast<size_t>(*width) * static_cast<size_t>(*height) * 3u, 0);
  while (cinfo.output_scanline < cinfo.output_height) {
    uint8_t* row = rgb->data() + static_cast<size_t>(cinfo.output_scanline) * static_cast<size_t>(*width) * 3u;
    JSAMPROW rows[1] = {row};
    jpeg_read_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return true;
}

bool gain_rgb(const std::string& path, const Rect& frame, const ChartSample& sample, float* r, float* g,
              float* b, std::string* error) {
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
  uhdr_mem_block_t* gain = st.error_code == UHDR_CODEC_OK ? uhdr_dec_get_gainmap_image(decoder) : nullptr;
  if (!gain || !gain->data || gain->data_sz == 0) {
    uhdr_release_decoder(decoder);
    if (error) *error = "encoded chart has no gain-map image";
    return false;
  }
  std::vector<uint8_t> map;
  int gw = 0;
  int gh = 0;
  const bool decoded = decode_jpeg_rgb(static_cast<const uint8_t*>(gain->data),
                                       static_cast<unsigned long>(gain->data_sz), &map, &gw, &gh, error);
  uhdr_release_decoder(decoder);
  if (!decoded) return false;
  const double fx = static_cast<double>(gw) / frame.w;
  const double fy = static_cast<double>(gh) / frame.h;
  const int x0 = std::clamp(static_cast<int>(std::floor((sample.x - frame.x) * fx)), 0, gw - 1);
  const int y0 = std::clamp(static_cast<int>(std::floor((sample.y - frame.y) * fy)), 0, gh - 1);
  const int x1 = std::clamp(static_cast<int>(std::ceil((sample.x + sample.w - frame.x) * fx)), x0 + 1, gw);
  const int y1 = std::clamp(static_cast<int>(std::ceil((sample.y + sample.h - frame.y) * fy)), y0 + 1, gh);
  double ar = 0;
  double ag = 0;
  double ab = 0;
  int n = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(gw) + static_cast<size_t>(x)) * 3u;
      ar += map[i];
      ag += map[i + 1];
      ab += map[i + 2];
      ++n;
    }
  }
  if (n == 0) {
    if (error) *error = "gain map sample was empty";
    return false;
  }
  *r = static_cast<float>(ar / n / 255.0);
  *g = static_cast<float>(ag / n / 255.0);
  *b = static_cast<float>(ab / n / 255.0);
  return true;
}

float hue_err(LinearRgb a, LinearRgb b) {
  const LinearRgb na = unit_max(a);
  const LinearRgb nb = unit_max(b);
  return std::max(std::fabs(na.r - nb.r), std::max(std::fabs(na.g - nb.g), std::fabs(na.b - nb.b)));
}

bool chromatic_pass(char channel, float r, float g, float b) {
  float dominant = b;
  float other1 = r;
  float other2 = g;
  if (channel == 'R') {
    dominant = r;
    other1 = g;
    other2 = b;
  } else if (channel == 'G') {
    dominant = g;
    other1 = r;
    other2 = b;
  }
  return dominant >= 0.50f && other1 <= 0.10f && other2 <= 0.10f &&
         (dominant - std::max(other1, other2)) >= 0.50f;
}

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

bool inside(const ChartSample& s, const Rect& f);

struct GradeView {
  Rect frame{0, 0, kWidth, kHeight};
  bool lightroom = false;
  std::string pass = "boost=full";
  std::string gain_path;
};

int grade(const std::vector<ChartSample>& samples, const std::vector<float>& rgb, int width, int height,
          const GradeView& view, float* exposure_out) {
  const float stop_tol = view.lightroom ? kLrStop : kEncStop;
  const float hue_tol = view.lightroom ? kLrHue : kEncHue;
  const Rect& f = view.frame;
  const double sx = static_cast<double>(width) / static_cast<double>(f.w);
  const double sy = static_cast<double>(height) / static_cast<double>(f.h);
  int outside = 0;
  struct Row {
    std::string group;
    std::string id;
    float exp_m = 0;
    float got_m = 0;
    float d_stop = 0;
    float d_hue = 0;
    bool gated = false;
    bool ok = false;
    bool monotonic = false;
    std::string series;
    int series_index = -1;
    bool exposure_ref = false;
  };
  std::vector<Row> rows;
  rows.reserve(samples.size());
  std::vector<std::string> groups;
  for (const ChartSample& s : samples) {
    if (!inside(s, f)) {
      ++outside;
      continue;
    }
    const int x0 = static_cast<int>(std::floor((s.x - f.x) * sx));
    const int y0 = static_cast<int>(std::floor((s.y - f.y) * sy));
    const int x1 = static_cast<int>(std::ceil((s.x + s.w - f.x) * sx));
    const int y1 = static_cast<int>(std::ceil((s.y + s.h - f.y) * sy));
    const LinearRgb got = sample_mean(rgb, width, height, x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0));
    const float exp_m = max3(s.expected);
    const float got_m = max3(got);
    float d_stop = 99.0f;
    if (exp_m > 1e-8f && got_m > 1e-8f) d_stop = std::fabs(std::log2(got_m / exp_m));
    else if (exp_m <= 1e-8f && got_m <= 1e-8f) d_stop = 0.0f;
    const bool hue_on = exp_m >= 0.01f;
    const float d_hue = hue_on ? hue_err(s.expected, got) : 0.0f;
    const bool gated = view.lightroom ? s.gate_lightroom : s.gate_encoder;
    const bool ok = d_stop <= stop_tol && (!hue_on || d_hue <= hue_tol);
    if (std::find(groups.begin(), groups.end(), s.group) == groups.end()) groups.push_back(s.group);
    rows.push_back({s.group, s.id, exp_m, got_m, d_stop, d_hue, gated, ok, s.monotonic, s.series, s.series_index,
                    s.exposure_ref});
  }

  std::printf("%-14s %-18s %10s %10s %7s %7s %s\n", "group", "id", "expected", "recovered", "d_stop", "d_hue",
              "result");
  int npass = 0;
  int nfail = 0;
  int nreport = 0;
  for (const Row& row : rows) {
    const char* result = !row.gated ? "REPORT" : (row.ok ? "PASS" : "FAIL");
    if (!row.gated) ++nreport;
    else if (row.ok) ++npass;
    else ++nfail;
    std::printf("%-14s %-18s %10.4f %10.4f %7.3f %7.3f %s\n", row.group.c_str(), row.id.c_str(), row.exp_m,
                row.got_m, row.d_stop, row.d_hue, result);
  }
  for (const std::string& group : groups) {
    int pass = 0;
    int fail = 0;
    int report = 0;
    float worst = -1.0f;
    std::string worst_id;
    for (const Row& row : rows) {
      if (row.group != group) continue;
      if (!row.gated) ++report;
      else if (row.ok) ++pass;
      else ++fail;
      if (row.d_stop > worst) {
        worst = row.d_stop;
        worst_id = row.id;
      }
    }
    std::printf("group %-14s pass=%d fail=%d report=%d worst=%s dstop=%.3f\n", group.c_str(), pass, fail, report,
                worst_id.c_str(), worst);
  }

  std::map<std::string, std::vector<size_t>> series;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].monotonic && !rows[i].series.empty() && rows[i].series_index >= 0) series[rows[i].series].push_back(i);
  }
  for (auto& item : series) {
    auto& idx = item.second;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return rows[a].series_index < rows[b].series_index; });
    bool ok = true;
    std::string where;
    for (size_t i = 1; i < idx.size(); ++i) {
      const Row& prev = rows[idx[i - 1]];
      const Row& cur = rows[idx[i]];
      if (!prev.gated || !cur.gated) continue;
      if (cur.got_m + 1e-8f < prev.got_m * std::exp2(-0.20f)) {
        ok = false;
        where = cur.id;
        break;
      }
    }
    std::printf("monotonic %s %s\n", item.first.c_str(), ok ? "PASS" : "FAIL");
    if (!ok) {
      std::cerr << "monotonic drop at " << where << "\n";
      ++nfail;
    }
  }

  std::vector<float> offsets;
  for (const Row& row : rows) {
    if (!row.exposure_ref || row.exp_m <= 1e-8f || row.got_m <= 1e-8f) continue;
    offsets.push_back(std::log2(row.got_m / row.exp_m));
  }
  if (offsets.empty()) {
    std::printf("exposure_offset_stops n/a\n");
    if (exposure_out) *exposure_out = 0.0f;
  } else {
    std::sort(offsets.begin(), offsets.end());
    const float med = offsets[offsets.size() / 2];
    std::printf("exposure_offset_stops %.3f\n", med);
    if (exposure_out) *exposure_out = med;
  }

  if (!view.gain_path.empty()) {
    for (const ChartSample& s : samples) {
      if (!s.chromatic || !inside(s, f)) continue;
      float r = 0;
      float g = 0;
      float b = 0;
      std::string err;
      const bool read = gain_rgb(view.gain_path, f, s, &r, &g, &b, &err);
      const bool ok = read && chromatic_pass(s.chromatic, r, g, b);
      const char* name = s.chromatic == 'R' ? "R2020" : s.chromatic == 'G' ? "G2020" : "B2020";
      std::printf("%s +4 gain RGB %.3f %.3f %.3f %s\n", name, r, g, b, ok ? "PASS" : "FAIL");
      if (!ok) {
        if (!read) std::cerr << err << "\n";
        else std::cerr << name << " +4 gain map is washed. The named channel must stay well above the others.\n";
        ++nfail;
      }
    }
  }

  const char* mode = view.lightroom ? "lightroom" : "encoder";
  const int gated = npass + nfail;
  std::printf("HDR_CHART %s %s %s gated=%d failed=%d outside=%d\n", mode, view.pass.c_str(),
              nfail ? "FAIL" : "PASS", gated, nfail, outside);
  std::fflush(stdout);
  return nfail ? 1 : 0;
}

bool is_tiff_path(const std::string& path) {
  std::ifstream in = open_input_binary(path);
  char mag[4] = {};
  in.read(mag, 4);
  return (mag[0] == 'I' && mag[1] == 'I') || (mag[0] == 'M' && mag[1] == 'M');
}

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
    } else if ((w < 1.0f || m.luma) && !inside_p3(s.expected)) {
      // Partial headroom and luma maps decide out-of-P3 clipping in the blend space.
      s.gate_encoder = false;
      s.gate_lightroom = false;
    } else {
      // Full headroom still clamps to max_content_boost; blended applies that ceiling.
      s.expected = blended(s.expected, m, w);
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

}  // namespace

int write_hdr_chart_main(const std::string& dir) {
  if (dir.empty()) {
    std::cerr << "--write-hdr-chart requires an output directory\n";
    return 1;
  }
  std::error_code ec;
  fs::create_directories(path_from_utf8(dir), ec);
  if (ec) {
    std::cerr << "could not create " << dir << ": " << ec.message() << "\n";
    return 1;
  }
  std::vector<ChartSample> samples;
  std::string err;
  if (!build_samples(&samples, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  std::vector<float> hdr;
  std::vector<uint8_t> sdr;
  render_chart(hdr, sdr, samples);

  const std::vector<uint8_t> tiff_icc = build_linear_rec2020_icc();
  const std::vector<uint8_t> jpeg_icc = build_display_p3_icc();
  if (!validate_linear_rec2020_icc(tiff_icc, &err) || !validate_display_p3_icc(jpeg_icc, &err)) {
    std::cerr << err << "\n";
    return 1;
  }

  const std::string tiff = join_dir(dir, "hdr-chart.tif");
  const std::string jpeg = join_dir(dir, "sdr-chart.jpg");
  const std::string man = join_dir(dir, "chart-manifest.json");
  if (!write_float_tiff(tiff, hdr, tiff_icc, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  std::vector<uint8_t> jpeg_bytes;
  if (!encode_p3_jpeg(sdr.data(), jpeg_icc, &jpeg_bytes, &err) || !write_bytes(jpeg, jpeg_bytes, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  {
    std::ofstream out = open_output_binary(man);
    if (!out) {
      std::cerr << "could not write " << man << "\n";
      return 1;
    }
    out << manifest_json(samples).dump(2) << "\n";
  }
  if (!embedded_profile_is(tiff, IccPrimaries::kRec2020, IccTransfer::kLinear, &err) ||
      !embedded_profile_is(jpeg, IccPrimaries::kDisplayP3, IccTransfer::kSrgb, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
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
  std::vector<float> reread;
  int rw = 0;
  int rh = 0;
  if (!load_tiff_rec2020(tiff, &reread, &rw, &rh, &err) ||
      !samples_match(reread, rw, rh, samples, "encoder TIFF reader", &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  std::cout << "Wrote " << tiff << "\n";
  std::cout << "Wrote " << jpeg << " (" << jpeg_bytes.size() << " bytes, Display P3 ICC)\n";
  std::cout << "Wrote " << man << " (" << samples.size() << " samples)\n";
  std::cout << kWidth << "x" << kHeight
            << " Deflate float TIFF, embedded profiles valid, portable readers match the manifest,"
               " gated samples fit 4:5\n";
  return 0;
}

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

}  // namespace uhdr_repack
