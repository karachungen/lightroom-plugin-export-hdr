#pragma once

#include "raw_image.h"

#include <cstdint>
#include <string>
#include <vector>

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
  /** When non-empty, used as the Ultra HDR JPEG primary (no second SDR encode). */
  std::vector<uint8_t> sdr_jpeg;
  /** uhdr_color_gamut_t for sdr_jpeg (0 = BT.709, 1 = Display P3). */
  int sdr_jpeg_cg = 1;
};

/** Encode Ultra HDR JPEG from HDR raw plus SDR raw and/or a JPEG primary. */
bool encode_ultra_hdr_jpeg(const RawImageHolder& hdr, const RawImageHolder* sdr,
                           const EncodeOptions& opt, const std::string& output_path,
                           std::string* error);

}  // namespace uhdr_repack
