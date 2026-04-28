#pragma once

#include "raw_image.h"

#include <string>

namespace uhdr_repack {

/** Load SDR base (JPEG/PNG/TIFF). Scaled to match HDR width/height. BT709 sRGB as 4:2:0 YCbCr for libultrahdr. */
bool load_sdr_base_raw(const std::string& path, unsigned target_width, unsigned target_height,
                       RawImageHolder* out, std::string* error);

}  // namespace uhdr_repack
