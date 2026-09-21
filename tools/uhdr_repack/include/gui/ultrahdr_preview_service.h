#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

struct DecodedHdrFrame {
  std::vector<uint16_t> rgba_half;
  std::vector<uint8_t> gainmap_jpeg;
  int width = 0;
  int height = 0;
  float display_boost = 1.0f;
  /** libultrahdr uhdr_color_gamut_t of the linear RGBA16F buffer. */
  int color_gamut = 0;
  bool gainmap_is_rgb = false;
  size_t jpeg_bytes = 0;
};

/** Decode the actual Ultra HDR output to linear RGBA16F for Final preview. */
bool decode_ultrahdr_preview(const std::string& path, float display_boost, DecodedHdrFrame* out,
                             std::string* error);

}  // namespace uhdr_repack
