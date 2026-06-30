#include "yuv_convert.h"

#include <cmath>
#include <memory>

namespace uhdr_repack {

namespace {

void rgb888_to_y_cb_cr_bt709(float r, float g, float b, float* y_out, float* cb_out,
                             float* cr_out) {
  constexpr float kr = 0.2126f;
  constexpr float kg = 0.7152f;
  constexpr float kb = 0.0722f;
  float y = kr * r + kg * g + kb * b;
  float cb = 128.f + (0.5f * (b - y)) / (1.f - kb);
  float cr = 128.f + (0.5f * (r - y)) / (1.f - kr);
  *y_out = y;
  *cb_out = cb;
  *cr_out = cr;
}

}  // namespace

bool rgba8888_to_yuv420_bt709(const uint8_t* rgba, unsigned w, unsigned h, uint8_t** plane_y,
                              uint8_t** plane_u, uint8_t** plane_v, std::string* error) {
  if (w % 2 || h % 2) {
    if (error) {
      *error = "SDR dimensions must be even for 4:2:0 YCbCr";
    }
    return false;
  }
  const unsigned cw = w / 2;
  const unsigned ch = h / 2;
  const size_t n_y = static_cast<size_t>(w) * static_cast<size_t>(h);
  const size_t n_c = static_cast<size_t>(cw) * static_cast<size_t>(ch);

  auto y_buf = std::unique_ptr<uint8_t[]>(new uint8_t[n_y]);
  auto u_buf = std::unique_ptr<uint8_t[]>(new uint8_t[n_c]);
  auto v_buf = std::unique_ptr<uint8_t[]>(new uint8_t[n_c]);

  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      size_t i = (static_cast<size_t>(y) * w + x) * 4;
      float rf = rgba[i];
      float gf = rgba[i + 1];
      float bf = rgba[i + 2];
      float yv, cb, cr;
      rgb888_to_y_cb_cr_bt709(rf, gf, bf, &yv, &cb, &cr);
      uint8_t y8 = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(yv))));
      y_buf[static_cast<size_t>(y) * w + x] = y8;
    }
  }

  for (unsigned by = 0; by < ch; ++by) {
    for (unsigned bx = 0; bx < cw; ++bx) {
      float acc_r = 0.f, acc_g = 0.f, acc_b = 0.f;
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          unsigned x = bx * 2 + static_cast<unsigned>(dx);
          unsigned yy = by * 2 + static_cast<unsigned>(dy);
          size_t i = (static_cast<size_t>(yy) * w + x) * 4;
          acc_r += rgba[i];
          acc_g += rgba[i + 1];
          acc_b += rgba[i + 2];
        }
      }
      acc_r *= 0.25f;
      acc_g *= 0.25f;
      acc_b *= 0.25f;
      float yv, cb, cr;
      rgb888_to_y_cb_cr_bt709(acc_r, acc_g, acc_b, &yv, &cb, &cr);
      uint8_t cb8 = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(cb))));
      uint8_t cr8 = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(cr))));
      size_t ci = static_cast<size_t>(by) * cw + bx;
      u_buf[ci] = cb8;
      v_buf[ci] = cr8;
    }
  }

  *plane_y = y_buf.release();
  *plane_u = u_buf.release();
  *plane_v = v_buf.release();
  return true;
}

}  // namespace uhdr_repack
