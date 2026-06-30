#include "half_float.h"

#include <cstring>

namespace uhdr_repack {

namespace {

uint16_t float_to_half_bits(float value) {
  if (!std::isfinite(value)) {
    if (std::isnan(value)) {
      return static_cast<uint16_t>(0x7E00);
    }
    return static_cast<uint16_t>(value < 0.f ? 0xFC00 : 0x7C00);
  }

  uint32_t f;
  std::memcpy(&f, &value, sizeof(f));
  const uint32_t sign = (f >> 16) & 0x8000u;
  const int32_t exp = static_cast<int32_t>((f >> 23) & 0xFFu) - 127 + 15;
  uint32_t mant = f & 0x7FFFFFu;

  if (exp <= 0) {
    if (exp < -10) {
      return static_cast<uint16_t>(sign);
    }
    mant |= 0x800000u;
    const int32_t shift = 14 - exp;
    uint32_t half_mant = mant >> shift;
    if ((mant >> (shift - 1)) & 1u) {
      half_mant += 1;
    }
    return static_cast<uint16_t>(sign | half_mant);
  }

  if (exp >= 31) {
    return static_cast<uint16_t>(sign | 0x7C00u);
  }

  uint16_t half = static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
                                        (mant >> 13));
  if ((mant >> 12) & 1u) {
    half += 1;
  }
  return half;
}

}  // namespace

uint16_t float_to_half(float value) {
#if defined(__APPLE__) && defined(__fp16)
  return static_cast<uint16_t>(static_cast<__fp16>(value));
#else
  return float_to_half_bits(value);
#endif
}

void float_rgba_to_half_rgba(const float* src, fp16_t* dst, size_t num_floats) {
  for (size_t i = 0; i < num_floats; ++i) {
    float f = src[i];
    if (!std::isfinite(f)) {
      f = 0.f;
    }
    dst[i] = float_to_half(f);
  }
}

void even_normalize(unsigned orig_w, unsigned orig_h, unsigned* w, unsigned* h) {
  *w = orig_w;
  *h = orig_h;
  if (*w % 2) {
    --(*w);
  }
  if (*h % 2) {
    --(*h);
  }
}

}  // namespace uhdr_repack
