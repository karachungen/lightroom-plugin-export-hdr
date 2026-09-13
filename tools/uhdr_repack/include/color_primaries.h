#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace uhdr_repack {

struct LinearRgb {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};

inline LinearRgb mul_mat3(const float m[9], LinearRgb c) {
  return {m[0] * c.r + m[1] * c.g + m[2] * c.b, m[3] * c.r + m[4] * c.g + m[5] * c.b,
          m[6] * c.r + m[7] * c.g + m[8] * c.b};
}

/** Linear Rec.2020 / BT.2100 RGB to linear Rec.709 / sRGB. */
inline LinearRgb rec2020_to_linear_srgb(LinearRgb value) {
  static const float k[9] = {1.660491f, -0.587641f, -0.072850f, -0.124550f, 1.132900f,
                             -0.008349f, -0.018151f, -0.100337f, 1.118488f};
  return mul_mat3(k, value);
}

/** Linear Rec.709 / sRGB to linear Rec.2020 / BT.2100. */
inline LinearRgb linear_srgb_to_rec2020(LinearRgb value) {
  static const float k[9] = {0.627404f, 0.329283f, 0.043313f, 0.069397f, 0.919540f,
                             0.011162f, 0.016391f, 0.088013f, 0.895595f};
  return mul_mat3(k, value);
}

inline LinearRgb linear_srgb_to_display_p3(LinearRgb value) {
  static const float k[9] = {0.822462f, 0.177538f, 0.000000f, 0.033194f, 0.966806f,
                             0.000000f, 0.017084f, 0.072397f, 0.910519f};
  return mul_mat3(k, value);
}

inline LinearRgb display_p3_to_linear_srgb(LinearRgb value) {
  static const float k[9] = {1.224940f, -0.224940f, 0.000000f, -0.042057f, 1.042057f,
                             0.000000f, -0.019638f, -0.078636f, 1.098274f};
  return mul_mat3(k, value);
}

inline LinearRgb tone_map_extended_reinhard(LinearRgb value) {
  const auto shoulder = [](float c) {
    if (c <= 1.0f) return c > 0.0f ? c : 0.0f;
    const float excess = c - 1.0f;
    return 1.0f + excess / (1.0f + excess);
  };
  return {shoulder(value.r), shoulder(value.g), shoulder(value.b)};
}

/** Shader/CPU gamut id: 0 = BT.709, 1 = Display P3, 2 = BT.2100. */
inline int display_gamut_id(int uhdr_cg) {
  // uhdr_color_gamut: UNSPECIFIED=-1, BT_709=0, DISPLAY_P3=1, BT_2100=2
  if (uhdr_cg == 2) return 2;
  if (uhdr_cg == 1) return 1;
  return 0;
}

inline LinearRgb decoded_hdr_to_linear_srgb(LinearRgb hdr, int uhdr_cg) {
  const int id = display_gamut_id(uhdr_cg);
  if (id == 2) return rec2020_to_linear_srgb(hdr);
  if (id == 1) return display_p3_to_linear_srgb(hdr);
  return hdr;
}

inline float srgb_eotf(float s) {
  if (s <= 0.04045f) return s / 12.92f;
  return std::pow((s + 0.055f) / 1.055f, 2.4f);
}

inline float srgb_oetf(float l) {
  if (l <= 0.0031308f) return 12.92f * l;
  return 1.055f * std::pow(std::max(l, 0.0f), 1.0f / 2.4f) - 0.055f;
}

/** In-place sRGB 8-bit RGBA → Display P3 8-bit (sRGB transfer). */
inline void srgb_rgba8888_to_display_p3(uint8_t* rgba, std::size_t pixel_count) {
  if (!rgba) return;
  for (std::size_t i = 0; i < pixel_count; ++i) {
    uint8_t* p = rgba + i * 4;
    LinearRgb lin{srgb_eotf(p[0] / 255.0f), srgb_eotf(p[1] / 255.0f),
                  srgb_eotf(p[2] / 255.0f)};
    LinearRgb p3 = linear_srgb_to_display_p3(lin);
    auto encode = [](float c) -> uint8_t {
      const float s = std::clamp(srgb_oetf(c), 0.0f, 1.0f);
      return static_cast<uint8_t>(std::lround(s * 255.0f));
    };
    p[0] = encode(p3.r);
    p[1] = encode(p3.g);
    p[2] = encode(p3.b);
  }
}

}  // namespace uhdr_repack
