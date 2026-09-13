#pragma once

#include "raw_image.h"

#include <string>

namespace uhdr_repack {

struct EncodeOptions {
  int base_quality = 95;
  int gainmap_quality = 95;
  int gainmap_scale = 1;
  float min_content_boost = 1.0f;
  float max_content_boost = 16.0f;
  float target_display_peak_nits = 3250.0f;
  bool monochrome_gainmap = false;
  std::string gainmap_in;
  std::string watermark_config;
  std::string metadata_patch;
};

/** Encode Ultra HDR JPEG from HDR + SDR raw buffers (caller frees RawImageHolder separately). */
bool encode_ultra_hdr_jpeg(const RawImageHolder& hdr, const RawImageHolder& sdr,
                           const EncodeOptions& opt, const std::string& output_path,
                           std::string* error);

}  // namespace uhdr_repack
