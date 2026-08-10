#pragma once

#include "raw_image.h"
#include "uhdr_encode.h"

#include <ultrahdr_api.h>

#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

struct CompatibilityGainmap {
  std::vector<float> gains_log2;
  std::vector<uint8_t> pixels;
  std::vector<uint8_t> jpeg;
  uhdr_gainmap_metadata_t metadata{};
  unsigned width = 0;
  unsigned height = 0;
  float min_gain_log2 = 0.0f;
  float max_gain_log2 = 0.0f;
};

/** Calculate, downsample, normalize, and JPEG-compress a scalar luminance gain map. */
bool build_compatibility_gainmap(const RawImageHolder& hdr, const RawImageHolder& sdr,
                                 const EncodeOptions& opt, CompatibilityGainmap* result,
                                 std::string* error);

}  // namespace uhdr_repack
