#pragma once

#include "color_primaries.h"

namespace uhdr_repack {

inline float hdr_sdr_white_scale(bool scene_referred, float sdr_white_nits) {
  if (!scene_referred) return 1.0f;
  return sdr_white_nits > 0.0f ? (80.0f / sdr_white_nits) : 1.0f;
}

inline float hdr_headroom_from_luminance(float max_luminance_nits, float sdr_white_nits) {
  const float white = sdr_white_nits > 0.0f ? sdr_white_nits : 203.0f;
  return max_luminance_nits > 0.0f ? max_luminance_nits / white : 1.0f;
}

}  // namespace uhdr_repack
