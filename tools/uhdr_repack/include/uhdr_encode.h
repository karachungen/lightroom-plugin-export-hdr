#pragma once

#include "raw_image.h"

#include <string>

namespace uhdr_repack {

struct EncodeOptions {
  int base_quality = 92;
  int gainmap_quality = 85;
  int gainmap_scale = 1;
  float min_content_boost = 1.0f;
  float max_content_boost = 1000.0f;
  float target_display_peak_nits = 1000.0f;
  bool monochrome_gainmap = false;
};

/** Encode Ultra HDR JPEG from HDR + SDR raw buffers (caller frees RawImageHolder separately). */
bool encode_ultra_hdr_jpeg(const RawImageHolder& hdr, const RawImageHolder& sdr,
                           const EncodeOptions& opt, const std::string& output_path,
                           std::string* error);

}  // namespace uhdr_repack
