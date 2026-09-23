#include "hdr_chart.h"

#include "color_primaries.h"
#include "encode_engine.h"
#include "half_float.h"
#include "path_io.h"
#include "tiff_input.h"

#include <ultrahdr_api.h>

#include <nlohmann/json.hpp>

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

constexpr int kPatch = 152;
constexpr int kGap = 16;
constexpr int kCols = 6;
constexpr int kRows = 10;
constexpr int kLeft = 120;
constexpr int kTop = 40;
constexpr int kRight = 328;
constexpr int kBottom = 36;
constexpr int kSharpGap = 20;
constexpr int kSharpH = 160;
constexpr int kSample = 48;
constexpr float kLabelLin = 0.25f;
constexpr float kBgLin = 0.06f;
constexpr float kStopTol = 0.15f;
constexpr float kHueTol = 0.05f;
constexpr float kCeilingBoost = 16.0f;

constexpr int kGridW = kCols * kPatch + (kCols - 1) * kGap;
constexpr int kGridH = kRows * kPatch + (kRows - 1) * kGap;
constexpr int kWidth = kLeft + kGridW + kRight;
constexpr int kHeight = kTop + kGridH + kSharpGap + kSharpH + kBottom;
static_assert(kWidth == 1440 && kHeight == 1920, "chart must be Instagram 3:4 at 1440 wide");
static_assert(kWidth * 4 == kHeight * 3, "chart must be 3:4");

struct RowDef {
  const char* name;
  bool p3_safe;
  LinearRgb unit;
};

LinearRgb unit_max(LinearRgb c) {
  const float m = std::max(c.r, std::max(c.g, c.b));
  if (m <= 1e-12f) return {0, 0, 0};
  return {c.r / m, c.g / m, c.b / m};
}

LinearRgb scale_rgb(LinearRgb c, float s) { return {c.r * s, c.g * s, c.b * s}; }

float max3(LinearRgb c) { return std::max(c.r, std::max(c.g, c.b)); }

RowDef make_p3_row(const char* name, LinearRgb srgb) {
  return {name, true, unit_max(linear_srgb_to_rec2020(srgb))};
}

const RowDef* rows() {
  static const RowDef kRowsDef[] = {
      make_p3_row("NEUTRAL", {1, 1, 1}),
      make_p3_row("RED", {1, 0, 0}),
      make_p3_row("GREEN", {0, 1, 0}),
      make_p3_row("BLUE", {0, 0, 1}),
      make_p3_row("YELLOW", {1, 1, 0}),
      make_p3_row("CYAN", {0, 1, 1}),
      make_p3_row("MAGENTA", {1, 0, 1}),
      {"R2020", false, {1, 0, 0}},
      {"G2020", false, {0, 1, 0}},
      {"B2020", false, {0, 0, 1}},
  };
  return kRowsDef;
}

int patch_x(int col) { return kLeft + col * (kPatch + kGap); }
int patch_y(int row) { return kTop + row * (kPatch + kGap); }

LinearRgb expected_hdr(const RowDef& row, int stop) {
  return scale_rgb(row.unit, std::ldexp(1.0f, stop));
}

LinearRgb sdr_rec2020(const RowDef& row) { return row.unit; }

void put_hdr(std::vector<float>& rgb, int x, int y, LinearRgb c) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
  const size_t i = (static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 3u;
  rgb[i] = c.r;
  rgb[i + 1] = c.g;
  rgb[i + 2] = c.b;
}

LinearRgb rec2020_to_sdr8(LinearRgb rec) {
  LinearRgb p3 = rec2020_to_display_p3(rec);
  p3.r = std::clamp(p3.r, 0.0f, 1.0f);
  p3.g = std::clamp(p3.g, 0.0f, 1.0f);
  p3.b = std::clamp(p3.b, 0.0f, 1.0f);
  return {srgb_oetf(p3.r), srgb_oetf(p3.g), srgb_oetf(p3.b)};
}

void put_sdr(std::vector<uint8_t>& rgba, int x, int y, LinearRgb rec) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
  const LinearRgb e = rec2020_to_sdr8(rec);
  const size_t i = (static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 4u;
  rgba[i] = static_cast<uint8_t>(std::lround(e.r * 255.0f));
  rgba[i + 1] = static_cast<uint8_t>(std::lround(e.g * 255.0f));
  rgba[i + 2] = static_cast<uint8_t>(std::lround(e.b * 255.0f));
  rgba[i + 3] = 255;
}

void fill_rect_hdr(std::vector<float>& rgb, int x, int y, int w, int h, LinearRgb c) {
  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) put_hdr(rgb, x + xx, y + yy, c);
  }
}

void fill_rect_sdr(std::vector<uint8_t>& rgba, int x, int y, int w, int h, LinearRgb rec) {
  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) put_sdr(rgba, x + xx, y + yy, rec);
  }
}

// 5x7 glyphs, 5 columns, LSB = top row.
const uint8_t* glyph(char ch) {
  static const uint8_t kEmpty[5] = {0, 0, 0, 0, 0};
  static const uint8_t kPlus[5] = {0x08, 0x08, 0x3E, 0x08, 0x08};
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
  static const uint8_t kG[5] = {0x3E, 0x41, 0x49, 0x49, 0x7A};
  static const uint8_t kH[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
  static const uint8_t kI[5] = {0x00, 0x41, 0x7F, 0x41, 0x00};
  static const uint8_t kL[5] = {0x7F, 0x40, 0x40, 0x40, 0x40};
  static const uint8_t kM[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
  static const uint8_t kN[5] = {0x7F, 0x04, 0x08, 0x10, 0x7F};
  static const uint8_t kO[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
  static const uint8_t kP[5] = {0x7F, 0x09, 0x09, 0x09, 0x06};
  static const uint8_t kR[5] = {0x7F, 0x09, 0x19, 0x29, 0x46};
  static const uint8_t kS[5] = {0x26, 0x49, 0x49, 0x49, 0x32};
  static const uint8_t kT[5] = {0x01, 0x01, 0x7F, 0x01, 0x01};
  static const uint8_t kU[5] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
  static const uint8_t kW[5] = {0x7F, 0x20, 0x18, 0x20, 0x7F};
  static const uint8_t kX[5] = {0x63, 0x14, 0x08, 0x14, 0x63};
  static const uint8_t kY[5] = {0x03, 0x04, 0x78, 0x04, 0x03};
  if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  switch (ch) {
    case '+':
      return kPlus;
    case '0':
      return k0;
    case '1':
      return k1;
    case '2':
      return k2;
    case '3':
      return k3;
    case '4':
      return k4;
    case '5':
      return k5;
    case '6':
      return k6;
    case '7':
      return k7;
    case '8':
      return k8;
    case '9':
      return k9;
    case 'A':
      return kA;
    case 'B':
      return kB;
    case 'C':
      return kC;
    case 'D':
      return kD;
    case 'E':
      return kE;
    case 'G':
      return kG;
    case 'H':
      return kH;
    case 'I':
      return kI;
    case 'L':
      return kL;
    case 'M':
      return kM;
    case 'N':
      return kN;
    case 'O':
      return kO;
    case 'P':
      return kP;
    case 'R':
      return kR;
    case 'S':
      return kS;
    case 'T':
      return kT;
    case 'U':
      return kU;
    case 'W':
      return kW;
    case 'X':
      return kX;
    case 'Y':
      return kY;
    default:
      return kEmpty;
  }
}

void draw_text(std::vector<float>& hdr, std::vector<uint8_t>& sdr, int x, int y, const char* text) {
  const LinearRgb ink{kLabelLin, kLabelLin, kLabelLin};
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

void draw_line_pairs(std::vector<float>& hdr, std::vector<uint8_t>& sdr, int x, int y, int w, int h,
                     int period, LinearRgb hi_hdr, LinearRgb hi_sdr) {
  const LinearRgb black{0, 0, 0};
  const int half = std::max(1, period);
  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) {
      const bool on = ((xx / half) % 2) == 0;
      const LinearRgb hc = on ? hi_hdr : black;
      const LinearRgb sc = on ? hi_sdr : black;
      put_hdr(hdr, x + xx, y + yy, hc);
      put_sdr(sdr, x + xx, y + yy, sc);
    }
  }
}

void draw_slant(std::vector<float>& hdr, std::vector<uint8_t>& sdr, int x, int y, int w, int h,
                LinearRgb hi_hdr, LinearRgb hi_sdr) {
  const LinearRgb black{0, 0, 0};
  for (int yy = 0; yy < h; ++yy) {
    const int edge = std::clamp(4 + yy / 4, 1, w - 2);
    for (int xx = 0; xx < w; ++xx) {
      const bool on = xx >= edge;
      put_hdr(hdr, x + xx, y + yy, on ? hi_hdr : black);
      put_sdr(sdr, x + xx, y + yy, on ? hi_sdr : black);
    }
  }
}

void render_chart(std::vector<float>& hdr, std::vector<uint8_t>& sdr) {
  hdr.assign(static_cast<size_t>(kWidth) * kHeight * 3u, 0.0f);
  sdr.assign(static_cast<size_t>(kWidth) * kHeight * 4u, 0);
  const LinearRgb bg{kBgLin, kBgLin, kBgLin};
  fill_rect_hdr(hdr, 0, 0, kWidth, kHeight, bg);
  fill_rect_sdr(sdr, 0, 0, kWidth, kHeight, bg);

  const RowDef* rdefs = rows();
  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kCols; ++col) {
      const int x = patch_x(col);
      const int y = patch_y(row);
      fill_rect_hdr(hdr, x, y, kPatch, kPatch, expected_hdr(rdefs[row], col));
      fill_rect_sdr(sdr, x, y, kPatch, kPatch, sdr_rec2020(rdefs[row]));
    }
    draw_text(hdr, sdr, 4, patch_y(row) + kPatch / 2 - 7, rdefs[row].name);
  }
  for (int col = 0; col < kCols; ++col) {
    const char* labels[] = {"+0", "+1", "+2", "+3", "+4", "+5"};
    draw_text(hdr, sdr, patch_x(col) + 8, 6, labels[col]);
  }

  const int sharp_y = kTop + kGridH + kSharpGap;
  const int panel_w = kGridW / 3;
  const int stops[3] = {0, 2, 4};
  const char* panel_names[3] = {"+0", "+2", "+4"};
  const LinearRgb sdr_white = expected_hdr(rdefs[0], 0);
  for (int p = 0; p < 3; ++p) {
    const int px = kLeft + p * panel_w;
    const LinearRgb hi = expected_hdr(rdefs[0], stops[p]);
    const int band_w = (panel_w - 16) / 4;
    const int band_h = kSharpH - 18;
    const int by = sharp_y + 16;
    draw_text(hdr, sdr, px, sharp_y, panel_names[p]);
    draw_line_pairs(hdr, sdr, px + 2, by, band_w, band_h, 1, hi, sdr_white);
    draw_line_pairs(hdr, sdr, px + 4 + band_w, by, band_w, band_h, 2, hi, sdr_white);
    draw_line_pairs(hdr, sdr, px + 6 + 2 * band_w, by, band_w, band_h, 4, hi, sdr_white);
    draw_slant(hdr, sdr, px + 8 + 3 * band_w, by, band_w, band_h, hi, sdr_white);
  }
}

void put_be16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}

void put_be32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v >> 24));
  b.push_back(static_cast<uint8_t>(v >> 16));
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}

void put_s15(std::vector<uint8_t>& b, float v) {
  put_be32(b, static_cast<uint32_t>(static_cast<int32_t>(std::lround(v * 65536.0f))));
}

void put_xyz_d50(std::vector<uint8_t>& b, float x, float y, float z) {
  const char t[] = {'X', 'Y', 'Z', ' '};
  b.insert(b.end(), t, t + 4);
  put_be32(b, 0);
  put_s15(b, x);
  put_s15(b, y);
  put_s15(b, z);
}

void put_para_srgb(std::vector<uint8_t>& b) {
  const char t[] = {'p', 'a', 'r', 'a'};
  b.insert(b.end(), t, t + 4);
  put_be32(b, 0);
  put_be16(b, 4);
  put_be16(b, 0);
  put_s15(b, 2.4f);
  put_s15(b, 1.0f / 1.055f);
  put_s15(b, 0.055f / 1.055f);
  put_s15(b, 1.0f / 12.92f);
  put_s15(b, 0.04045f);
  put_s15(b, 0.0f);
  put_s15(b, 0.0f);
}

void put_desc(std::vector<uint8_t>& b, const char* ascii) {
  const char t[] = {'d', 'e', 's', 'c'};
  b.insert(b.end(), t, t + 4);
  put_be32(b, 0);
  const uint32_t n = static_cast<uint32_t>(std::strlen(ascii) + 1);
  put_be32(b, n);
  b.insert(b.end(), ascii, ascii + n);
  b.insert(b.end(), 67, 0);  // Unicode / script code padding used by v2 desc
}

void mul3(const float a[9], const float b[9], float o[9]) {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    }
  }
}

std::vector<uint8_t> display_p3_icc() {
  // Display P3 RGB→XYZ D65, then Bradford to D50 PCS.
  const float rgb_to_xyz_d65[9] = {0.48657095f, 0.26566769f, 0.19821729f, 0.22897456f, 0.69173852f,
                                   0.07928691f, 0.00000000f, 0.04511338f, 1.04394437f};
  const float bradford[9] = {0.8951f, 0.2664f, -0.1614f, -0.7502f, 1.7135f, 0.0367f, 0.0389f,
                             -0.0685f, 1.0296f};
  const float bradford_inv[9] = {0.9869929f, -0.1470543f, 0.1599627f, 0.4323053f, 0.5183603f,
                                 0.0492912f, -0.0085287f, 0.0400428f, 0.9684867f};
  const float d65[3] = {0.95047f, 1.0f, 1.08883f};
  const float d50[3] = {0.96422f, 1.0f, 0.82521f};
  float src_lms[3] = {bradford[0] * d65[0] + bradford[1] * d65[1] + bradford[2] * d65[2],
                      bradford[3] * d65[0] + bradford[4] * d65[1] + bradford[5] * d65[2],
                      bradford[6] * d65[0] + bradford[7] * d65[1] + bradford[8] * d65[2]};
  float dst_lms[3] = {bradford[0] * d50[0] + bradford[1] * d50[1] + bradford[2] * d50[2],
                      bradford[3] * d50[0] + bradford[4] * d50[1] + bradford[5] * d50[2],
                      bradford[6] * d50[0] + bradford[7] * d50[1] + bradford[8] * d50[2]};
  float scale[9] = {dst_lms[0] / src_lms[0], 0, 0, 0, dst_lms[1] / src_lms[1], 0, 0, 0,
                    dst_lms[2] / src_lms[2]};
  float tmp[9];
  float adapt[9];
  mul3(scale, bradford, tmp);
  mul3(bradford_inv, tmp, adapt);
  float m[9];
  mul3(adapt, rgb_to_xyz_d65, m);

  const char* desc = "Display P3";
  const char* cprt = "CC0";

  std::vector<uint8_t> rxyz, gxyz, bxyz, wtpt, rtrc, descb, cprtb;
  put_xyz_d50(rxyz, m[0], m[3], m[6]);
  put_xyz_d50(gxyz, m[1], m[4], m[7]);
  put_xyz_d50(bxyz, m[2], m[5], m[8]);
  put_xyz_d50(wtpt, d50[0], d50[1], d50[2]);
  put_para_srgb(rtrc);
  put_desc(descb, desc);
  put_desc(cprtb, cprt);

  const int ntags = 9;
  const uint32_t tag_table = 128;
  const uint32_t data0 = tag_table + 4 + static_cast<uint32_t>(ntags) * 12;
  struct Tag {
    uint32_t sig;
    std::vector<uint8_t>* data;
  };
  Tag tags[] = {
      {0x63707274, &cprtb}, {0x64657363, &descb}, {0x77747074, &wtpt},
      {0x7258595A, &rxyz},  {0x6758595A, &gxyz},  {0x6258595A, &bxyz},
      {0x72545243, &rtrc},  {0x67545243, &rtrc},  {0x62545243, &rtrc},
  };
  uint32_t off = data0;
  uint32_t offsets[9];
  for (int i = 0; i < ntags; ++i) {
    offsets[i] = off;
    off += static_cast<uint32_t>(tags[i].data->size());
    off = (off + 3u) & ~3u;
  }
  const uint32_t size = off;
  std::vector<uint8_t> icc(size, 0);
  auto w32 = [&](uint32_t at, uint32_t v) {
    icc[at] = static_cast<uint8_t>(v >> 24);
    icc[at + 1] = static_cast<uint8_t>(v >> 16);
    icc[at + 2] = static_cast<uint8_t>(v >> 8);
    icc[at + 3] = static_cast<uint8_t>(v);
  };
  w32(0, size);
  icc[4] = 'a';
  icc[5] = 'c';
  icc[6] = 's';
  icc[7] = 'p';
  icc[8] = 'm';
  icc[9] = 'n';
  icc[10] = 't';
  icc[11] = 'r';
  icc[12] = 'R';
  icc[13] = 'G';
  icc[14] = 'B';
  icc[15] = ' ';
  icc[16] = 'X';
  icc[17] = 'Y';
  icc[18] = 'Z';
  icc[19] = ' ';
  icc[36] = 'a';
  icc[37] = 'c';
  icc[38] = 's';
  icc[39] = 'p';
  w32(68, 0x0000F6D6);
  w32(72, 0x00010000);
  w32(76, 0x0000D32D);
  icc[80] = 'u';
  icc[81] = 'h';
  icc[82] = 'd';
  icc[83] = 'r';
  w32(128, static_cast<uint32_t>(ntags));
  for (int i = 0; i < ntags; ++i) {
    const uint32_t e = 132 + static_cast<uint32_t>(i) * 12;
    w32(e, tags[i].sig);
    w32(e + 4, offsets[i]);
    w32(e + 8, static_cast<uint32_t>(tags[i].data->size()));
    std::memcpy(icc.data() + offsets[i], tags[i].data->data(), tags[i].data->size());
  }
  return icc;
}

void put_curv_linear(std::vector<uint8_t>& b) {
  const char t[] = {'c', 'u', 'r', 'v'};
  b.insert(b.end(), t, t + 4);
  put_be32(b, 0);
  put_be32(b, 0);
}

std::vector<uint8_t> linear_rec2020_icc() {
  const float rgb_to_xyz_d65[9] = {0.63695805f, 0.14461690f, 0.16888098f, 0.26270021f, 0.67799807f,
                                   0.05930172f, 0.00000000f, 0.02807269f, 1.06098506f};
  const float bradford[9] = {0.8951f, 0.2664f, -0.1614f, -0.7502f, 1.7135f, 0.0367f, 0.0389f,
                             -0.0685f, 1.0296f};
  const float bradford_inv[9] = {0.9869929f, -0.1470543f, 0.1599627f, 0.4323053f, 0.5183603f,
                                 0.0492912f, -0.0085287f, 0.0400428f, 0.9684867f};
  const float d65[3] = {0.95047f, 1.0f, 1.08883f};
  const float d50[3] = {0.96422f, 1.0f, 0.82521f};
  float src_lms[3] = {bradford[0] * d65[0] + bradford[1] * d65[1] + bradford[2] * d65[2],
                      bradford[3] * d65[0] + bradford[4] * d65[1] + bradford[5] * d65[2],
                      bradford[6] * d65[0] + bradford[7] * d65[1] + bradford[8] * d65[2]};
  float dst_lms[3] = {bradford[0] * d50[0] + bradford[1] * d50[1] + bradford[2] * d50[2],
                      bradford[3] * d50[0] + bradford[4] * d50[1] + bradford[5] * d50[2],
                      bradford[6] * d50[0] + bradford[7] * d50[1] + bradford[8] * d50[2]};
  float scale[9] = {dst_lms[0] / src_lms[0], 0, 0, 0, dst_lms[1] / src_lms[1], 0, 0, 0,
                    dst_lms[2] / src_lms[2]};
  float tmp[9];
  float adapt[9];
  mul3(scale, bradford, tmp);
  mul3(bradford_inv, tmp, adapt);
  float m[9];
  mul3(adapt, rgb_to_xyz_d65, m);

  const char* desc = "Linear Rec.2020";
  const char* cprt = "CC0";
  std::vector<uint8_t> rxyz, gxyz, bxyz, wtpt, rtrc, descb, cprtb;
  put_xyz_d50(rxyz, m[0], m[3], m[6]);
  put_xyz_d50(gxyz, m[1], m[4], m[7]);
  put_xyz_d50(bxyz, m[2], m[5], m[8]);
  put_xyz_d50(wtpt, d50[0], d50[1], d50[2]);
  put_curv_linear(rtrc);
  put_desc(descb, desc);
  put_desc(cprtb, cprt);

  const int ntags = 9;
  const uint32_t tag_table = 128;
  const uint32_t data0 = tag_table + 4 + static_cast<uint32_t>(ntags) * 12;
  struct Tag {
    uint32_t sig;
    std::vector<uint8_t>* data;
  };
  Tag tags[] = {
      {0x63707274, &cprtb}, {0x64657363, &descb}, {0x77747074, &wtpt},
      {0x7258595A, &rxyz},  {0x6758595A, &gxyz},  {0x6258595A, &bxyz},
      {0x72545243, &rtrc},  {0x67545243, &rtrc},  {0x62545243, &rtrc},
  };
  uint32_t off = data0;
  uint32_t offsets[9];
  for (int i = 0; i < ntags; ++i) {
    offsets[i] = off;
    off += static_cast<uint32_t>(tags[i].data->size());
    off = (off + 3u) & ~3u;
  }
  const uint32_t size = off;
  std::vector<uint8_t> icc(size, 0);
  auto w32 = [&](uint32_t at, uint32_t v) {
    icc[at] = static_cast<uint8_t>(v >> 24);
    icc[at + 1] = static_cast<uint8_t>(v >> 16);
    icc[at + 2] = static_cast<uint8_t>(v >> 8);
    icc[at + 3] = static_cast<uint8_t>(v);
  };
  w32(0, size);
  icc[4] = 'a';
  icc[5] = 'c';
  icc[6] = 's';
  icc[7] = 'p';
  icc[8] = 'm';
  icc[9] = 'n';
  icc[10] = 't';
  icc[11] = 'r';
  icc[12] = 'R';
  icc[13] = 'G';
  icc[14] = 'B';
  icc[15] = ' ';
  icc[16] = 'X';
  icc[17] = 'Y';
  icc[18] = 'Z';
  icc[19] = ' ';
  icc[36] = 'a';
  icc[37] = 'c';
  icc[38] = 's';
  icc[39] = 'p';
  w32(68, 0x0000F6D6);
  w32(72, 0x00010000);
  w32(76, 0x0000D32D);
  icc[80] = 'u';
  icc[81] = 'h';
  icc[82] = 'd';
  icc[83] = 'r';
  w32(128, static_cast<uint32_t>(ntags));
  for (int i = 0; i < ntags; ++i) {
    const uint32_t e = 132 + static_cast<uint32_t>(i) * 12;
    w32(e, tags[i].sig);
    w32(e + 4, offsets[i]);
    w32(e + 8, static_cast<uint32_t>(tags[i].data->size()));
    std::memcpy(icc.data() + offsets[i], tags[i].data->data(), tags[i].data->size());
  }
  return icc;
}

struct JpegErr {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void jpeg_fail(j_common_ptr cinfo) {
  longjmp(reinterpret_cast<JpegErr*>(cinfo->err)->jump, 1);
}

bool encode_p3_jpeg(const uint8_t* rgba, std::vector<uint8_t>* jpeg, std::string* error) {
  jpeg_compress_struct cinfo{};
  JpegErr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_fail;
  unsigned char* outbuf = nullptr;
  unsigned long outsize = 0;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_compress(&cinfo);
    if (outbuf) free(outbuf);
    if (error) *error = "libjpeg failed to compress chart JPEG";
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
  const std::vector<uint8_t> icc = display_p3_icc();
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

void put_u16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}

void put_u32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v >> 16));
  b.push_back(static_cast<uint8_t>(v >> 24));
}

void ifd_entry(std::vector<uint8_t>& b, uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
  put_u16(b, tag);
  put_u16(b, type);
  put_u32(b, count);
  put_u32(b, value);
}

bool write_float_tiff(const std::string& path, const std::vector<float>& rgb, std::string* error) {
  const std::vector<uint8_t> icc = linear_rec2020_icc();
  const uint32_t ntags = 12;
  const uint32_t ifd = 8;
  const uint32_t extra = ifd + 2 + ntags * 12 + 4;
  const uint32_t bits_off = extra;
  const uint32_t fmt_off = extra + 8;
  const uint32_t icc_off = extra + 16;
  const uint32_t data_off = (icc_off + static_cast<uint32_t>(icc.size()) + 1u) & ~1u;
  const uint32_t nbytes = static_cast<uint32_t>(kWidth) * static_cast<uint32_t>(kHeight) * 12u;

  std::vector<uint8_t> file;
  file.reserve(data_off + nbytes);
  file.push_back('I');
  file.push_back('I');
  put_u16(file, 42);
  put_u32(file, ifd);
  put_u16(file, static_cast<uint16_t>(ntags));
  ifd_entry(file, 256, 3, 1, static_cast<uint32_t>(kWidth));
  ifd_entry(file, 257, 3, 1, static_cast<uint32_t>(kHeight));
  ifd_entry(file, 258, 3, 3, bits_off);
  ifd_entry(file, 259, 3, 1, 1);
  ifd_entry(file, 262, 3, 1, 2);
  ifd_entry(file, 273, 4, 1, data_off);
  ifd_entry(file, 277, 3, 1, 3);
  ifd_entry(file, 278, 3, 1, static_cast<uint32_t>(kHeight));
  ifd_entry(file, 279, 4, 1, nbytes);
  ifd_entry(file, 284, 3, 1, 1);
  ifd_entry(file, 339, 3, 3, fmt_off);
  ifd_entry(file, 34675, 7, static_cast<uint32_t>(icc.size()), icc_off);
  put_u32(file, 0);
  put_u16(file, 32);
  put_u16(file, 32);
  put_u16(file, 32);
  put_u16(file, 0);
  put_u16(file, 3);
  put_u16(file, 3);
  put_u16(file, 3);
  put_u16(file, 0);
  file.insert(file.end(), icc.begin(), icc.end());
  if (file.size() > data_off) {
    if (error) *error = "internal TIFF header size mismatch";
    return false;
  }
  file.resize(data_off, 0);
  if (file.size() != data_off) {
    if (error) *error = "internal TIFF header size mismatch";
    return false;
  }
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(rgb.data());
  file.insert(file.end(), raw, raw + nbytes);

  std::ofstream out = open_output_binary(path);
  if (!out) {
    if (error) *error = "could not write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  return static_cast<bool>(out);
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

bool verify_tiff_plus4(const std::string& path, std::string* error) {
  RawImageHolder hdr;
  if (!load_hdr_tiff_raw(path, &hdr, error)) return false;
  const uhdr_raw_image_t& r = hdr.ref();
  if (r.w != static_cast<unsigned>(kWidth) || r.h != static_cast<unsigned>(kHeight) ||
      !r.planes[UHDR_PLANE_PACKED]) {
    if (error) *error = "reloaded TIFF size mismatch";
    return false;
  }
  const int col = 4;
  const int row = 0;
  const int x = patch_x(col) + kPatch / 2;
  const int y = patch_y(row) + kPatch / 2;
  const auto* src = static_cast<const uint16_t*>(r.planes[UHDR_PLANE_PACKED]);
  const size_t i = (static_cast<size_t>(y) * r.stride[UHDR_PLANE_PACKED] + static_cast<size_t>(x)) * 4u;
  const float rv = half_to_float(src[i]);
  const float gv = half_to_float(src[i + 1]);
  const float bv = half_to_float(src[i + 2]);
  const float recovered = std::max(rv, std::max(gv, bv));
  if (std::fabs(recovered - 16.0f) > 0.5f) {
    if (error) {
      *error = "reloaded +4 NEUTRAL is " + std::to_string(recovered) + " (expected 16). The TIFF was not read as linear Rec.2020.";
    }
    return false;
  }

  const int red_row = 7;
  const int rx = patch_x(col) + kPatch / 2;
  const int ry = patch_y(red_row) + kPatch / 2;
  const size_t ri = (static_cast<size_t>(ry) * r.stride[UHDR_PLANE_PACKED] + static_cast<size_t>(rx)) * 4u;
  const float rr = half_to_float(src[ri]);
  const float rg = half_to_float(src[ri + 1]);
  const float rb = half_to_float(src[ri + 2]);
  if (std::fabs(rr - 16.0f) > 0.5f || std::fabs(rg) > 0.5f || std::fabs(rb) > 0.5f) {
    if (error) {
      *error = "reloaded +4 R2020 is (" + std::to_string(rr) + ", " + std::to_string(rg) + ", " +
               std::to_string(rb) + ") expected (16, 0, 0). The TIFF was color-managed away from linear Rec.2020.";
    }
    return false;
  }
  return true;
}

nlohmann::json manifest_json() {
  nlohmann::json rows_j = nlohmann::json::array();
  const RowDef* rdefs = rows();
  for (int row = 0; row < kRows; ++row) {
    rows_j.push_back({{"name", rdefs[row].name},
                      {"p3_safe", rdefs[row].p3_safe},
                      {"x", patch_x(0)},
                      {"y", patch_y(row)},
                      {"unit", {rdefs[row].unit.r, rdefs[row].unit.g, rdefs[row].unit.b}}});
  }
  return {{"width", kWidth},
          {"height", kHeight},
          {"patch", kPatch},
          {"gap", kGap},
          {"sample", kSample},
          {"stops", {0, 1, 2, 3, 4, 5}},
          {"max_content_boost", kCeilingBoost},
          {"rows", rows_j}};
}

std::string join_dir(const std::string& dir, const char* name) {
  return (path_from_utf8(dir) / name).u8string();
}

LinearRgb to_rec2020(LinearRgb c, int cg) {
  if (cg == 2) return c;
  if (cg == 1) return display_p3_to_rec2020(c);
  return linear_srgb_to_rec2020(c);
}

bool decode_uhdr_linear(const std::string& path, float display_boost, std::vector<float>* rgba,
                        int* width, int* height, int* cg, std::string* error) {
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
  *cg = static_cast<int>(image->cg);
  rgba->assign(static_cast<size_t>(image->w) * image->h * 4u, 0.0f);
  const auto* src = static_cast<const uint16_t*>(image->planes[UHDR_PLANE_PACKED]);
  for (unsigned y = 0; y < image->h; ++y) {
    for (unsigned x = 0; x < image->w; ++x) {
      const size_t si = (static_cast<size_t>(y) * image->stride[UHDR_PLANE_PACKED] + x) * 4u;
      const size_t di = (static_cast<size_t>(y) * image->w + x) * 4u;
      (*rgba)[di] = half_to_float(src[si]);
      (*rgba)[di + 1] = half_to_float(src[si + 1]);
      (*rgba)[di + 2] = half_to_float(src[si + 2]);
      (*rgba)[di + 3] = half_to_float(src[si + 3]);
    }
  }
  uhdr_release_decoder(decoder);
  return true;
}

LinearRgb sample_mean(const std::vector<float>& rgba, int width, int height, int x0, int y0, int cg) {
  const int x1 = x0 + kSample;
  const int y1 = y0 + kSample;
  LinearRgb acc{};
  int n = 0;
  for (int y = y0; y < y1 && y < height; ++y) {
    for (int x = x0; x < x1 && x < width; ++x) {
      const size_t i = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4u;
      const LinearRgb rec = to_rec2020({rgba[i], rgba[i + 1], rgba[i + 2]}, cg);
      acc.r += rec.r;
      acc.g += rec.g;
      acc.b += rec.b;
      ++n;
    }
  }
  if (n == 0) return {};
  return {acc.r / n, acc.g / n, acc.b / n};
}

float hue_err(LinearRgb a, LinearRgb b) {
  const LinearRgb na = unit_max(a);
  const LinearRgb nb = unit_max(b);
  return std::max(std::fabs(na.r - nb.r), std::max(std::fabs(na.g - nb.g), std::fabs(na.b - nb.b)));
}

bool decode_jpeg_rgb(const uint8_t* data, unsigned long size, std::vector<uint8_t>* rgb, int* width,
                     int* height, std::string* error) {
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

bool r2020_plus4_gain_chromatic(const std::string& path, float* red, float* green, float* blue,
                                std::string* error) {
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
  std::vector<uint8_t> rgb;
  int gw = 0;
  int gh = 0;
  const bool decoded =
      decode_jpeg_rgb(static_cast<const uint8_t*>(gain->data), static_cast<unsigned long>(gain->data_sz),
                      &rgb, &gw, &gh, error);
  uhdr_release_decoder(decoder);
  if (!decoded) return false;
  if (gw < 2 || gh < 2) {
    if (error) *error = "gain map is empty";
    return false;
  }

  const int col = 4;
  const int row = 7;
  const int sx = patch_x(col) + (kPatch - kSample) / 2;
  const int sy = patch_y(row) + (kPatch - kSample) / 2;
  const int x0 = std::clamp(sx * gw / kWidth, 0, gw - 1);
  const int y0 = std::clamp(sy * gh / kHeight, 0, gh - 1);
  const int x1 = std::clamp(x0 + std::max(1, kSample * gw / kWidth), x0 + 1, gw);
  const int y1 = std::clamp(y0 + std::max(1, kSample * gh / kHeight), y0 + 1, gh);
  double ar = 0;
  double ag = 0;
  double ab = 0;
  int n = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(gw) + static_cast<size_t>(x)) * 3u;
      ar += rgb[i];
      ag += rgb[i + 1];
      ab += rgb[i + 2];
      ++n;
    }
  }
  if (n == 0) {
    if (error) *error = "gain map sample was empty";
    return false;
  }
  *red = static_cast<float>(ar / n / 255.0);
  *green = static_cast<float>(ag / n / 255.0);
  *blue = static_cast<float>(ab / n / 255.0);
  return true;
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

  std::vector<float> hdr;
  std::vector<uint8_t> sdr;
  render_chart(hdr, sdr);

  std::string err;
  const std::string tiff = join_dir(dir, "hdr-chart.tif");
  const std::string jpeg = join_dir(dir, "sdr-chart.jpg");
  const std::string man = join_dir(dir, "chart-manifest.json");
  if (!write_float_tiff(tiff, hdr, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  std::vector<uint8_t> jpeg_bytes;
  if (!encode_p3_jpeg(sdr.data(), &jpeg_bytes, &err) || !write_bytes(jpeg, jpeg_bytes, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  {
    std::ofstream out = open_output_binary(man);
    if (!out) {
      std::cerr << "could not write " << man << "\n";
      return 1;
    }
    out << manifest_json().dump(2) << "\n";
  }
  if (!verify_tiff_plus4(tiff, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  std::cout << "Wrote " << tiff << "\n";
  std::cout << "Wrote " << jpeg << " (" << jpeg_bytes.size() << " bytes, Display P3 ICC)\n";
  std::cout << "Wrote " << man << "\n";
  std::cout << kWidth << "x" << kHeight
            << " linear Rec.2020, +4 NEUTRAL reloaded as 16, +4 R2020 reloaded as (16, 0, 0)\n";
  return 0;
}

int check_hdr_chart_main(const std::string& dir) {
  if (dir.empty()) {
    std::cerr << "--check-hdr-chart requires a chart directory\n";
    return 1;
  }
  const std::string tiff = join_dir(dir, "hdr-chart.tif");
  const std::string jpeg = join_dir(dir, "sdr-chart.jpg");
  const std::string out = join_dir(dir, "chart-uhdr.jpg");
  if (!fs::exists(path_from_utf8(tiff)) || !fs::exists(path_from_utf8(jpeg))) {
    std::cerr << "missing chart files; run --write-hdr-chart " << dir << " first\n";
    return 1;
  }

  // NEON and SSE libjpeg-turbo Huffman tables are not byte-identical.
  // The chart snapshot is the scalar encoder output on both platforms.
#ifdef _WIN32
  _putenv_s("JSIMD_FORCENONE", "1");
#else
  setenv("JSIMD_FORCENONE", "1", 1);
#endif

  EncodeRequest req;
  req.hdr_tiff = tiff;
  req.base_path = jpeg;
  req.out_path = out;
  req.options.base_quality = 95;
  req.options.gainmap_quality = 95;
  req.options.gainmap_scale = 1;
  req.options.min_content_boost = 1.0f;
  req.options.max_content_boost = kCeilingBoost;
  req.options.target_display_peak_nits = 3250.0f;
  req.options.monochrome_gainmap = false;

  std::string err;
  const int enc = encode_from_paths(req, &err);
  if (enc != 0) {
    std::cerr << "encode failed: " << err << "\n";
    return enc;
  }

  std::vector<float> decoded;
  int dw = 0;
  int dh = 0;
  int cg = 0;
  if (!decode_uhdr_linear(out, kCeilingBoost, &decoded, &dw, &dh, &cg, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  if (dw != kWidth || dh != kHeight) {
    std::cerr << "decoded size " << dw << "x" << dh << " != chart " << kWidth << "x" << kHeight
              << "\n";
    return 1;
  }

  const RowDef* rdefs = rows();
  int failures = 0;
  std::printf("%-8s %4s %10s %10s %8s %8s %s\n", "row", "stop", "expected", "recovered", "d_stop",
              "d_hue", "result");
  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kCols; ++col) {
      // Compare to the written HDR. Color map metadata is boost 16 / +4, but
      // libultrahdr still reconstructs +5 from the gain map in current builds.
      const LinearRgb want = expected_hdr(rdefs[row], col);
      const int sx = patch_x(col) + (kPatch - kSample) / 2;
      const int sy = patch_y(row) + (kPatch - kSample) / 2;
      const LinearRgb got = sample_mean(decoded, dw, dh, sx, sy, cg);
      const float exp_m = max3(want);
      const float got_m = max3(got);
      float d_stop = 99.0f;
      if (exp_m > 1e-8f && got_m > 1e-8f) {
        d_stop = std::fabs(std::log2(got_m / exp_m));
      } else if (exp_m <= 1e-8f && got_m <= 1e-8f) {
        d_stop = 0.0f;
      }
      const float d_hue = hue_err(want, got);
      const float ceiling = max3(scale_rgb(rdefs[row].unit, kCeilingBoost));
      const bool collapsed = got_m < 0.01f * ceiling;
      const bool stop_ok = d_stop <= kStopTol;
      const bool hue_ok = d_hue <= kHueTol;
      const char* result = "REPORT";
      if (rdefs[row].p3_safe) {
        const bool ok = stop_ok && hue_ok;
        result = ok ? "PASS" : "FAIL";
        if (!ok) ++failures;
      } else if (collapsed) {
        result = "FAIL";
        ++failures;
      }
      std::printf("%-8s %+4d %10.4f %10.4f %8.3f %8.3f %s\n", rdefs[row].name, col, exp_m, got_m,
                  d_stop, d_hue, result);
    }
  }
  std::fflush(stdout);

  float gain_r = 0;
  float gain_g = 0;
  float gain_b = 0;
  if (!r2020_plus4_gain_chromatic(out, &gain_r, &gain_g, &gain_b, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  // Code 1 is the map's own peak (here past +4, because +5 is in the chart). A washed
  // map boosts every channel together; a chromatic map leaves green near zero.
  const bool chromatic = gain_r >= 0.50f && gain_g <= 0.10f && (gain_r - gain_g) >= 0.50f;
  std::printf("R2020 +4 gain RGB %.3f %.3f %.3f %s\n", gain_r, gain_g, gain_b,
              chromatic ? "PASS" : "FAIL");
  if (!chromatic) {
    std::cerr << "R2020 +4 gain map is gray (washed). Red must stay well above green, and green near none.\n";
    ++failures;
  }

  if (failures) {
    std::cerr << failures << " gated patch(es) failed\n";
    return 1;
  }
  std::cout << "All gated P3-safe patches recovered within " << kStopTol << " stop and " << kHueTol
            << " hue. R2020 +4 gain stayed chromatic.\n";
  return 0;
}

}  // namespace uhdr_repack
