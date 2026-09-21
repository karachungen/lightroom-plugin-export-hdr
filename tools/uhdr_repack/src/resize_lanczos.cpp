#include "resize_lanczos.h"

#include "half_float.h"
#include "yuv_convert.h"

#include <ultrahdr_api.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace uhdr_repack {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kLanczosA = 3;

float sinc(float x) {
  if (std::fabs(x) < 1e-8f) {
    return 1.0f;
  }
  const float pix = x * kPi;
  return std::sin(pix) / pix;
}

float lanczos3(float x) {
  const float ax = std::fabs(x);
  if (ax >= static_cast<float>(kLanczosA)) {
    return 0.0f;
  }
  return sinc(x) * sinc(x / static_cast<float>(kLanczosA));
}

float srgb8_to_linear(uint8_t u) {
  const float s = static_cast<float>(u) / 255.0f;
  if (s <= 0.04045f) {
    return s / 12.92f;
  }
  return std::pow((s + 0.055f) / 1.055f, 2.4f);
}

uint8_t linear_to_srgb8(float l) {
  l = std::max(0.0f, l);
  const float s = l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
  return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, std::round(s * 255.0f))));
}

void resize_plane_lanczos(const float* src, unsigned src_w, unsigned src_h, float* dst,
                          unsigned dst_w, unsigned dst_h) {
  std::vector<float> tmp(static_cast<size_t>(src_h) * dst_w);

  const float scale_x = static_cast<float>(src_w) / static_cast<float>(dst_w);
  const float scale_y = static_cast<float>(src_h) / static_cast<float>(dst_h);

  for (unsigned y = 0; y < src_h; ++y) {
    for (unsigned x = 0; x < dst_w; ++x) {
      const float src_x = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
      const int x_center = static_cast<int>(std::floor(src_x));
      float sum = 0.0f;
      float wsum = 0.0f;
      for (int k = x_center - kLanczosA + 1; k <= x_center + kLanczosA; ++k) {
        const int sx = std::clamp(k, 0, static_cast<int>(src_w) - 1);
        const float w = lanczos3(src_x - static_cast<float>(k));
        if (w == 0.0f) {
          continue;
        }
        sum += src[static_cast<size_t>(y) * src_w + static_cast<unsigned>(sx)] * w;
        wsum += w;
      }
      tmp[static_cast<size_t>(y) * dst_w + x] = wsum > 1e-8f ? sum / wsum : 0.0f;
    }
  }

  for (unsigned y = 0; y < dst_h; ++y) {
    const float src_y = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
    const int y_center = static_cast<int>(std::floor(src_y));
    for (unsigned x = 0; x < dst_w; ++x) {
      float sum = 0.0f;
      float wsum = 0.0f;
      for (int k = y_center - kLanczosA + 1; k <= y_center + kLanczosA; ++k) {
        const int sy = std::clamp(k, 0, static_cast<int>(src_h) - 1);
        const float w = lanczos3(src_y - static_cast<float>(k));
        if (w == 0.0f) {
          continue;
        }
        sum += tmp[static_cast<size_t>(static_cast<unsigned>(sy)) * dst_w + x] * w;
        wsum += w;
      }
      dst[static_cast<size_t>(y) * dst_w + x] = wsum > 1e-8f ? sum / wsum : 0.0f;
    }
  }
}

void unsharp_rgb8(uint8_t* rgba, unsigned w, unsigned h) {
  const size_t n = static_cast<size_t>(w) * h;
  std::vector<float> luma(n);
  std::vector<float> blur(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* p = rgba + i * 4;
    luma[i] = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
  }
  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      float acc = 0.0f;
      float wsum = 0.0f;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int sx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(w) - 1);
          const int sy = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(h) - 1);
          const float gw = (dx == 0 && dy == 0) ? 4.0f : (dx == 0 || dy == 0) ? 2.0f : 1.0f;
          acc += luma[static_cast<size_t>(sy) * w + static_cast<unsigned>(sx)] * gw;
          wsum += gw;
        }
      }
      blur[static_cast<size_t>(y) * w + x] = acc / wsum;
    }
  }
  constexpr float amount = 0.65f;
  for (size_t i = 0; i < n; ++i) {
    const float detail = luma[i] - blur[i];
    const float scale = luma[i] > 1.0f ? (luma[i] + amount * detail) / luma[i] : 1.0f;
    uint8_t* p = rgba + i * 4;
    for (int c = 0; c < 3; ++c) {
      const float v = static_cast<float>(p[c]) * scale;
      p[c] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, std::round(v))));
    }
  }
}

bool copy_hdr(const RawImageHolder& src, RawImageHolder* dst, std::string* error) {
  const uhdr_raw_image_t& s = src.ref();
  if (s.fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || !s.planes[UHDR_PLANE_PACKED]) {
    if (error) {
      *error = "HDR resize expects RGBA half-float";
    }
    return false;
  }
  dst->reset();
  const size_t n = static_cast<size_t>(s.w) * s.h * 4;
  auto* packed = static_cast<fp16_t*>(std::malloc(n * sizeof(fp16_t)));
  if (!packed) {
    if (error) {
      *error = "HDR clone alloc failed";
    }
    return false;
  }
  const auto* srcp = static_cast<const fp16_t*>(s.planes[UHDR_PLANE_PACKED]);
  const unsigned stride = s.stride[UHDR_PLANE_PACKED];
  for (unsigned y = 0; y < s.h; ++y) {
    std::memcpy(packed + static_cast<size_t>(y) * s.w * 4, srcp + static_cast<size_t>(y) * stride * 4,
                static_cast<size_t>(s.w) * 4 * sizeof(fp16_t));
  }
  uhdr_raw_image_t& d = dst->ref();
  d = s;
  d.planes[UHDR_PLANE_PACKED] = packed;
  d.stride[UHDR_PLANE_PACKED] = s.w;
  d.planes[1] = nullptr;
  d.planes[2] = nullptr;
  return true;
}

}  // namespace

bool resize_hdr_lanczos(const RawImageHolder& src, unsigned dst_w, unsigned dst_h,
                        RawImageHolder* dst, std::string* error) {
  if (!dst || dst_w < 2 || dst_h < 2 || (dst_w % 2) || (dst_h % 2)) {
    if (error) {
      *error = "HDR resize requires even destination size >= 2";
    }
    return false;
  }
  const uhdr_raw_image_t& s = src.ref();
  if (s.w == dst_w && s.h == dst_h) {
    return copy_hdr(src, dst, error);
  }
  if (s.fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || !s.planes[UHDR_PLANE_PACKED]) {
    if (error) {
      *error = "HDR resize expects RGBA half-float";
    }
    return false;
  }

  const auto* srcp = static_cast<const fp16_t*>(s.planes[UHDR_PLANE_PACKED]);
  const unsigned stride = s.stride[UHDR_PLANE_PACKED];
  const size_t src_n = static_cast<size_t>(s.w) * s.h;
  const size_t dst_n = static_cast<size_t>(dst_w) * dst_h;
  std::vector<float> src_ch(src_n);
  std::vector<float> dst_ch(dst_n);
  std::vector<fp16_t> packed(dst_n * 4);

  for (int c = 0; c < 4; ++c) {
    for (unsigned y = 0; y < s.h; ++y) {
      for (unsigned x = 0; x < s.w; ++x) {
        src_ch[static_cast<size_t>(y) * s.w + x] =
            half_to_float(srcp[(static_cast<size_t>(y) * stride + x) * 4 + static_cast<unsigned>(c)]);
      }
    }
    resize_plane_lanczos(src_ch.data(), s.w, s.h, dst_ch.data(), dst_w, dst_h);
    for (size_t i = 0; i < dst_n; ++i) {
      packed[i * 4 + static_cast<size_t>(c)] = float_to_half(dst_ch[i]);
    }
  }

  dst->reset();
  auto* outp = static_cast<fp16_t*>(std::malloc(packed.size() * sizeof(fp16_t)));
  if (!outp) {
    if (error) {
      *error = "HDR resize alloc failed";
    }
    return false;
  }
  std::memcpy(outp, packed.data(), packed.size() * sizeof(fp16_t));
  uhdr_raw_image_t& d = dst->ref();
  std::memset(&d, 0, sizeof(d));
  d.fmt = s.fmt;
  d.cg = s.cg;
  d.ct = s.ct;
  d.range = s.range;
  d.w = dst_w;
  d.h = dst_h;
  d.planes[UHDR_PLANE_PACKED] = outp;
  d.stride[UHDR_PLANE_PACKED] = dst_w;
  return true;
}

bool resize_sdr_lanczos_sharpen(const RawImageHolder& src, unsigned dst_w, unsigned dst_h,
                                RawImageHolder* dst, std::string* error) {
  if (!dst || dst_w < 2 || dst_h < 2 || (dst_w % 2) || (dst_h % 2)) {
    if (error) {
      *error = "SDR resize requires even destination size >= 2";
    }
    return false;
  }
  const uhdr_raw_image_t& s = src.ref();
  if (s.fmt != UHDR_IMG_FMT_12bppYCbCr420 || !s.planes[UHDR_PLANE_Y] || !s.planes[UHDR_PLANE_U] ||
      !s.planes[UHDR_PLANE_V]) {
    if (error) {
      *error = "SDR resize expects 4:2:0 YCbCr";
    }
    return false;
  }

  std::vector<uint8_t> src_rgba(static_cast<size_t>(s.w) * s.h * 4);
  if (!yuv420_bt601_to_rgba8888(static_cast<const uint8_t*>(s.planes[UHDR_PLANE_Y]),
                                static_cast<const uint8_t*>(s.planes[UHDR_PLANE_U]),
                                static_cast<const uint8_t*>(s.planes[UHDR_PLANE_V]), s.w, s.h,
                                s.stride[UHDR_PLANE_Y], s.stride[UHDR_PLANE_U],
                                s.stride[UHDR_PLANE_V], src_rgba.data(), error)) {
    return false;
  }

  std::vector<uint8_t> dst_rgba(static_cast<size_t>(dst_w) * dst_h * 4, 255);
  if (s.w == dst_w && s.h == dst_h) {
    dst_rgba = src_rgba;
  } else {
    const size_t src_n = static_cast<size_t>(s.w) * s.h;
    const size_t dst_n = static_cast<size_t>(dst_w) * dst_h;
    std::vector<float> src_ch(src_n);
    std::vector<float> dst_ch(dst_n);
    for (int c = 0; c < 3; ++c) {
      for (size_t i = 0; i < src_n; ++i) {
        src_ch[i] = srgb8_to_linear(src_rgba[i * 4 + static_cast<size_t>(c)]);
      }
      resize_plane_lanczos(src_ch.data(), s.w, s.h, dst_ch.data(), dst_w, dst_h);
      for (size_t i = 0; i < dst_n; ++i) {
        dst_rgba[i * 4 + static_cast<size_t>(c)] = linear_to_srgb8(dst_ch[i]);
      }
    }
  }
  unsharp_rgb8(dst_rgba.data(), dst_w, dst_h);

  uint8_t* py = nullptr;
  uint8_t* pu = nullptr;
  uint8_t* pv = nullptr;
  if (!rgba8888_to_yuv420_bt601(dst_rgba.data(), dst_w, dst_h, &py, &pu, &pv, error)) {
    return false;
  }

  dst->reset();
  uhdr_raw_image_t& d = dst->ref();
  std::memset(&d, 0, sizeof(d));
  d.fmt = UHDR_IMG_FMT_12bppYCbCr420;
  d.cg = s.cg;
  d.ct = s.ct;
  d.range = s.range;
  d.w = dst_w;
  d.h = dst_h;
  d.planes[UHDR_PLANE_Y] = py;
  d.planes[UHDR_PLANE_U] = pu;
  d.planes[UHDR_PLANE_V] = pv;
  d.stride[UHDR_PLANE_Y] = dst_w;
  d.stride[UHDR_PLANE_U] = dst_w / 2;
  d.stride[UHDR_PLANE_V] = dst_w / 2;
  return true;
}

}  // namespace uhdr_repack
