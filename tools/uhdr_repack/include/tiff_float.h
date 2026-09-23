#pragma once

#include "raw_image.h"
#include "slice_plan.h"

#include <string>

namespace uhdr_repack {

/**
 * Copy an uncompressed 32-bit IEEE RGB TIFF into linear Rec.2020 half-float when the
 * file is untagged or tagged "Linear Rec.2020".
 * @return 1 loaded, 0 not this kind of TIFF (caller uses the platform decoder), -1 error.
 */
int load_identity_rec2020_tiff(const std::string& path, RawImageHolder* out, std::string* error,
                               unsigned master_w = 0, unsigned master_h = 0,
                               const CropRect* crop = nullptr, unsigned dst_w = 0,
                               unsigned dst_h = 0);

/** Even-normalized size of an identity float TIFF. Same return codes as the loader. */
int probe_identity_rec2020_tiff(const std::string& path, unsigned* master_w, unsigned* master_h,
                                std::string* error);

}  // namespace uhdr_repack
