#include "yuv_convert.h"

#include <cmath>
#include <memory>

namespace uhdr_repack {

namespace {

struct LumaCoeffs {
  float kr;
  float kg;
  float kb;
};

constexpr LumaCoeffs kBt709{0.2126f, 0.7152f, 0.0722f};
constexpr LumaCoeffs kBt601{0.299f, 0.587f, 0.114f};

void rgb888_to_y_cb_cr(float r, float g, float b, const LumaCoeffs& k, float* y_out, float* cb_out,
                       float* cr_out) {
  float y = k.kr * r + k.kg * g + k.kb * b;
  float cb = 128.f + (0.5f * (b - y)) / (1.f - k.kb);
  float cr = 128.f + (0.5f * (r - y)) / (1.f - k.kr);
  *y_out = y;
  *cb_out = cb;
  *cr_out = cr;
}

bool rgba8888_to_yuv420(const uint8_t* rgba, unsigned w, unsigned h, const LumaCoeffs& k,
                        uint8_t** plane_y, uint8_t** plane_u, uint8_t** plane_v, std::string* error) {
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
      float yv, cb, cr;
      rgb888_to_y_cb_cr(rgba[i], rgba[i + 1], rgba[i + 2], k, &yv, &cb, &cr);
      y_buf[static_cast<size_t>(y) * w + x] =
          static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(yv))));
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
      rgb888_to_y_cb_cr(acc_r, acc_g, acc_b, k, &yv, &cb, &cr);
      size_t ci = static_cast<size_t>(by) * cw + bx;
      u_buf[ci] = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(cb))));
      v_buf[ci] = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(cr))));
    }
  }

  *plane_y = y_buf.release();
  *plane_u = u_buf.release();
  *plane_v = v_buf.release();
  return true;
}

bool yuv420_to_rgba8888(const uint8_t* plane_y, const uint8_t* plane_u, const uint8_t* plane_v,
                        unsigned w, unsigned h, unsigned stride_y, unsigned stride_u,
                        unsigned stride_v, const LumaCoeffs& k, uint8_t* rgba, std::string* error) {
  if (!plane_y || !plane_u || !plane_v || !rgba || w < 2 || h < 2 || (w % 2) || (h % 2)) {
    if (error) {
      *error = "YUV to RGBA requires even dimensions and valid planes";
    }
    return false;
  }
  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      const float yv = static_cast<float>(plane_y[static_cast<size_t>(y) * stride_y + x]);
      const unsigned cx = x / 2;
      const unsigned cy = y / 2;
      const float cb = static_cast<float>(plane_u[static_cast<size_t>(cy) * stride_u + cx]) - 128.f;
      const float cr = static_cast<float>(plane_v[static_cast<size_t>(cy) * stride_v + cx]) - 128.f;
      const float r = yv + 2.f * (1.f - k.kr) * cr;
      const float b = yv + 2.f * (1.f - k.kb) * cb;
      const float g = (yv - k.kr * r - k.kb * b) / k.kg;
      const size_t i = (static_cast<size_t>(y) * w + x) * 4;
      rgba[i] = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(r))));
      rgba[i + 1] = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(g))));
      rgba[i + 2] = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(b))));
      rgba[i + 3] = 255;
    }
  }
  return true;
}

}  // namespace

bool rgba8888_to_yuv420_bt709(const uint8_t* rgba, unsigned w, unsigned h, uint8_t** plane_y,
                              uint8_t** plane_u, uint8_t** plane_v, std::string* error) {
  return rgba8888_to_yuv420(rgba, w, h, kBt709, plane_y, plane_u, plane_v, error);
}

bool rgba8888_to_yuv420_bt601(const uint8_t* rgba, unsigned w, unsigned h, uint8_t** plane_y,
                              uint8_t** plane_u, uint8_t** plane_v, std::string* error) {
  return rgba8888_to_yuv420(rgba, w, h, kBt601, plane_y, plane_u, plane_v, error);
}

bool yuv420_bt709_to_rgba8888(const uint8_t* plane_y, const uint8_t* plane_u, const uint8_t* plane_v,
                              unsigned w, unsigned h, unsigned stride_y, unsigned stride_u,
                              unsigned stride_v, uint8_t* rgba, std::string* error) {
  return yuv420_to_rgba8888(plane_y, plane_u, plane_v, w, h, stride_y, stride_u, stride_v, kBt709,
                            rgba, error);
}

bool yuv420_bt601_to_rgba8888(const uint8_t* plane_y, const uint8_t* plane_u, const uint8_t* plane_v,
                              unsigned w, unsigned h, unsigned stride_y, unsigned stride_u,
                              unsigned stride_v, uint8_t* rgba, std::string* error) {
  return yuv420_to_rgba8888(plane_y, plane_u, plane_v, w, h, stride_y, stride_u, stride_v, kBt601,
                            rgba, error);
}

}  // namespace uhdr_repack
