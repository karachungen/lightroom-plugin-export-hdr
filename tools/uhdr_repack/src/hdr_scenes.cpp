#include "hdr_scenes.h"

#include "chart_io.h"
#include "color_primaries.h"
#include "icc_profile.h"
#include "path_io.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace uhdr_repack {
namespace fs = std::filesystem;

namespace {

constexpr int kW = 1080;
constexpr int kH = 1350;

float smoothstep(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float mix(float a, float b, float t) { return a + (b - a) * t; }

LinearRgb mix(LinearRgb a, LinearRgb b, float t) {
  return {mix(a.r, b.r, t), mix(a.g, b.g, t), mix(a.b, b.b, t)};
}

LinearRgb max_rgb(LinearRgb a, LinearRgb b) {
  return {std::max(a.r, b.r), std::max(a.g, b.g), std::max(a.b, b.b)};
}

LinearRgb add_rgb(LinearRgb a, LinearRgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }

LinearRgb scale_rgb(LinearRgb c, float s) { return {c.r * s, c.g * s, c.b * s}; }

float dist2(float dx, float dy) { return dx * dx + dy * dy; }

float segment_dist(float x, float y, float x0, float y0, float x1, float y1) {
  const float vx = x1 - x0;
  const float vy = y1 - y0;
  const float wx = x - x0;
  const float wy = y - y0;
  const float vv = vx * vx + vy * vy;
  float t = vv > 1e-12f ? (wx * vx + wy * vy) / vv : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  return std::sqrt(dist2(x - (x0 + t * vx), y - (y0 + t * vy)));
}

float rect_outline_dist(float x, float y, float x0, float y0, float x1, float y1) {
  const float cx = 0.5f * (x0 + x1);
  const float cy = 0.5f * (y0 + y1);
  const float hx = 0.5f * (x1 - x0);
  const float hy = 0.5f * (y1 - y0);
  const float dx = std::fabs(x - cx) - hx;
  const float dy = std::fabs(y - cy) - hy;
  const float outside = std::sqrt(dist2(std::max(dx, 0.0f), std::max(dy, 0.0f)));
  const float inside = std::min(std::max(dx, dy), 0.0f);
  return std::fabs(outside + inside);
}

void put_pixel(std::vector<float>& hdr, std::vector<uint8_t>& sdr, int x, int y, LinearRgb p3) {
  const LinearRgb rec = display_p3_to_rec2020(p3);
  const size_t hi = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 3u;
  hdr[hi] = rec.r;
  hdr[hi + 1] = rec.g;
  hdr[hi + 2] = rec.b;
  const LinearRgb s = sdr_p3(rec);
  const size_t si = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
  sdr[si] = static_cast<uint8_t>(std::lround(srgb_oetf(s.r) * 255.0f));
  sdr[si + 1] = static_cast<uint8_t>(std::lround(srgb_oetf(s.g) * 255.0f));
  sdr[si + 2] = static_cast<uint8_t>(std::lround(srgb_oetf(s.b) * 255.0f));
  sdr[si + 3] = 255;
}

LinearRgb sunset_pixel(int x, int y) {
  const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kW);
  const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kH);
  const LinearRgb top{0.015f, 0.03f, 0.10f};
  const LinearRgb horizon{1.20f, 0.42f, 0.08f};
  LinearRgb c = mix(top, horizon, smoothstep(0.0f, 0.58f, v));

  const float sx = 0.5f * static_cast<float>(kW);
  const float sy = 0.50f * static_cast<float>(kH);
  const float d = std::sqrt(dist2(static_cast<float>(x) + 0.5f - sx, static_cast<float>(y) + 0.5f - sy));
  const float core = 1.0f - smoothstep(66.0f, 72.0f, d);
  c = add_rgb(c, scale_rgb({8.0f, 5.6f, 2.4f}, core));
  c = add_rgb(c, scale_rgb({1.8f, 0.9f, 0.25f}, std::exp(-d * d / (2.0f * 160.0f * 160.0f))));

  const float xf = static_cast<float>(x) + 0.5f;
  const float yr = 0.62f * static_cast<float>(kH) + 40.0f * std::sin(xf / 180.0f) +
                   25.0f * std::sin(xf / 67.0f + 1.3f);
  const float t = std::clamp(static_cast<float>(y) + 0.5f - yr + 0.5f, 0.0f, 1.0f);
  const LinearRgb hill{0.010f, 0.008f, 0.012f};
  return mix(c, hill, t);
}

void add_tube(LinearRgb* glow_sum, LinearRgb* core_max, float dist, LinearRgb colour) {
  *glow_sum = add_rgb(*glow_sum, scale_rgb(colour, 0.25f * std::exp(-dist * dist / (2.0f * 18.0f * 18.0f))));
  const float t = std::clamp(4.5f - dist, 0.0f, 1.0f);
  *core_max = max_rgb(*core_max, scale_rgb(colour, t));
}

LinearRgb neon_pixel(int x, int y) {
  const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kH);
  LinearRgb c{0.010f, 0.010f, 0.015f};
  c = add_rgb(c, scale_rgb({0.02f, 0.0f, 0.03f}, smoothstep(0.7f, 1.0f, v)));

  const float px = static_cast<float>(x) + 0.5f;
  const float py = static_cast<float>(y) + 0.5f;
  LinearRgb glow{};
  LinearRgb core{};

  add_tube(&glow, &core, rect_outline_dist(px, py, 140.0f, 180.0f, 940.0f, 560.0f), {6.0f, 0.25f, 5.0f});

  const float ring = std::fabs(std::sqrt(dist2(px - 540.0f, py - 860.0f)) - 210.0f);
  add_tube(&glow, &core, ring, {0.2f, 5.0f, 6.0f});

  static const float zig[][2] = {{160, 1180}, {330, 1060}, {500, 1180}, {670, 1060}, {840, 1180}, {920, 1120}};
  float zig_d = 1e9f;
  for (int i = 0; i < 5; ++i) {
    zig_d = std::min(zig_d, segment_dist(px, py, zig[i][0], zig[i][1], zig[i + 1][0], zig[i + 1][1]));
  }
  add_tube(&glow, &core, zig_d, {6.0f, 2.2f, 0.15f});

  c = add_rgb(c, glow);
  return max_rgb(c, core);
}

LinearRgb pastel_pixel(int x, int y) {
  const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kH);
  LinearRgb c = mix({0.55f, 0.65f, 0.85f}, {0.92f, 0.82f, 0.76f}, smoothstep(0.0f, 1.0f, v));

  const float dx = static_cast<float>(x) + 0.5f - 540.0f;
  const float dy = static_cast<float>(y) + 0.5f - 700.0f;
  constexpr float rx = 260.0f;
  constexpr float ry = 340.0f;
  const float r_norm = std::sqrt((dx / rx) * (dx / rx) + (dy / ry) * (dy / ry));
  const float signed_d = (r_norm - 1.0f) * std::min(rx, ry);
  const float t = 1.0f - smoothstep(-15.0f, 15.0f, signed_d);
  c = mix(c, {0.78f, 0.56f, 0.46f}, t);

  const float pts[][2] = {{470, 600}, {620, 610}, {545, 820}};
  for (const auto& p : pts) {
    const float d2 = dist2(static_cast<float>(x) + 0.5f - p[0], static_cast<float>(y) + 0.5f - p[1]);
    c = add_rgb(c, scale_rgb({1.2f, 1.1f, 1.0f}, std::exp(-d2 / (2.0f * 25.0f * 25.0f))));
  }
  return c;
}

void render_scene(std::vector<float>& hdr, std::vector<uint8_t>& sdr, LinearRgb (*pixel)(int, int)) {
  hdr.assign(static_cast<size_t>(kW) * kH * 3u, 0.0f);
  sdr.assign(static_cast<size_t>(kW) * kH * 4u, 0);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      put_pixel(hdr, sdr, x, y, pixel(x, y));
    }
  }
}

std::string join_dir(const std::string& dir, const std::string& name) {
  return (path_from_utf8(dir) / name).u8string();
}

bool write_scene_pair(const std::string& dir, const char* name, LinearRgb (*pixel)(int, int),
                      const std::vector<uint8_t>& tiff_icc, const std::vector<uint8_t>& jpeg_icc,
                      std::string* error) {
  std::vector<float> hdr;
  std::vector<uint8_t> sdr;
  render_scene(hdr, sdr, pixel);
  const std::string tiff = join_dir(dir, std::string(name) + "-hdr.tif");
  const std::string jpeg = join_dir(dir, std::string(name) + "-sdr.jpg");
  if (!write_float_tiff(tiff, hdr, kW, kH, tiff_icc, error)) return false;
  std::vector<uint8_t> jpeg_bytes;
  if (!encode_p3_jpeg(sdr.data(), kW, kH, jpeg_icc, &jpeg_bytes, error) ||
      !write_bytes(jpeg, jpeg_bytes, error)) {
    return false;
  }
  std::cout << "Wrote " << tiff << "\n";
  std::cout << "Wrote " << jpeg << "\n";
  return true;
}

}  // namespace

int write_hdr_scenes_main(const std::string& dir) {
  if (dir.empty()) {
    std::cerr << "--write-hdr-scenes requires an output directory\n";
    return 1;
  }
  std::error_code ec;
  fs::create_directories(path_from_utf8(dir), ec);
  if (ec) {
    std::cerr << "could not create " << dir << ": " << ec.message() << "\n";
    return 1;
  }
  const std::vector<uint8_t> tiff_icc = build_linear_rec2020_icc();
  const std::vector<uint8_t> jpeg_icc = build_display_p3_icc();
  std::string err;
  struct Scene {
    const char* name;
    LinearRgb (*pixel)(int, int);
  };
  const Scene scenes[] = {{"sunset", sunset_pixel}, {"neon", neon_pixel}, {"pastel", pastel_pixel}};
  for (const Scene& s : scenes) {
    if (!write_scene_pair(dir, s.name, s.pixel, tiff_icc, jpeg_icc, &err)) {
      std::cerr << err << "\n";
      return 1;
    }
  }
  return 0;
}

}  // namespace uhdr_repack
