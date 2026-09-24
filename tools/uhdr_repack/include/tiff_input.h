#pragma once

#include "raw_image.h"
#include "slice_plan.h"

#include <string>

namespace uhdr_repack {

/** Even-normalized master size of an HDR TIFF (no pixel buffer allocated). */
bool probe_hdr_tiff_even_size(const std::string& path, unsigned* master_w, unsigned* master_h,
                              std::string* error);

/**
 * Load an HDR TIFF into linear Rec.2020 RGBA half-float.
 * load_float_tiff_portable reads float TIFFs (uncompressed or Deflate, linear profile) on both
 * platforms. Anything else is read with Core Image on Mac or WIC on Windows, after one stderr line.
 * When crop is non-null, master_w/master_h must be the even-normalized full frame size.
 */
bool load_hdr_tiff_raw(const std::string& path, RawImageHolder* out, std::string* error,
                       unsigned master_w = 0, unsigned master_h = 0,
                       const CropRect* crop = nullptr, unsigned dst_w = 0, unsigned dst_h = 0);

}  // namespace uhdr_repack
