#pragma once

#include "raw_image.h"
#include "slice_plan.h"

#include <string>

namespace uhdr_repack {

/**
 * Load SDR base (JPEG/PNG/TIFF). Scaled to master_width×master_height, optionally cropped.
 * Output is BT709 sRGB as 4:2:0 YCbCr for libultrahdr.
 */
bool load_sdr_base_raw(const std::string& path, unsigned master_width, unsigned master_height,
                       RawImageHolder* out, std::string* error,
                       const CropRect* crop = nullptr);

/** Compatibility scalar decode, with embedded SDR ICC converted to the declared sRGB space. */
bool load_sdr_base_raw_compatibility(const std::string& path, unsigned master_width,
                                     unsigned master_height, RawImageHolder* out,
                                     std::string* error, const CropRect* crop = nullptr);

}  // namespace uhdr_repack
