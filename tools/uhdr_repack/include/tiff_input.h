#pragma once

#include "raw_image.h"

#include <string>

namespace uhdr_repack {

/** Load Lightroom HDR TIFF via Core Image expandToHDR into linear RGBA half-float for libultrahdr. */
bool load_hdr_tiff_raw(const std::string& path, RawImageHolder* out, std::string* error);

}  // namespace uhdr_repack
