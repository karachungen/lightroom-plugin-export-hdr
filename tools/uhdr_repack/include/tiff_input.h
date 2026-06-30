#pragma once

#include "raw_image.h"
#include "slice_plan.h"

#include <string>

namespace uhdr_repack {

/** Even-normalized master size of an HDR TIFF (no pixel buffer allocated). */
bool probe_hdr_tiff_even_size(const std::string& path, unsigned* master_w, unsigned* master_h,
                              std::string* error);

/**
 * Load Lightroom HDR TIFF via Core Image expandToHDR into linear RGBA half-float.
 * When crop is non-null, master_w/master_h must be the even-normalized full frame size.
 */
bool load_hdr_tiff_raw(const std::string& path, RawImageHolder* out, std::string* error,
                       unsigned master_w = 0, unsigned master_h = 0,
                       const CropRect* crop = nullptr);

}  // namespace uhdr_repack
