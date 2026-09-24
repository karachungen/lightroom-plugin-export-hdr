#pragma once

#include "icc_profile.h"
#include "raw_image.h"
#include "slice_plan.h"

#include <cstdint>
#include <string>

namespace uhdr_repack {

/** What the portable TIFF reader saw. supported=false means an OS decoder reads the file; why says what was missing. */
struct FloatTiffInfo {
  bool is_tiff = false;
  bool supported = false;
  std::string why;
  unsigned width = 0;
  unsigned height = 0;
  uint16_t compression = 1;
  uint16_t predictor = 1;
  bool big_endian = false;
  bool has_icc = false;
  IccClass icc;
};

bool describe_float_tiff(const std::string& path, FloatTiffInfo* info, std::string* error);

/**
 * Read a strip TIFF of 32-bit IEEE float RGB(A), uncompressed or Deflate with predictor 1 or 3,
 * untagged or with a linear Rec.2020, Display P3, or sRGB-primaries profile, into linear
 * Rec.2020 half-float. Same code on macOS and Windows.
 * @return 1 loaded, 0 not this kind of TIFF (caller uses the OS decoder), -1 error.
 */
int load_float_tiff_portable(const std::string& path, RawImageHolder* out, std::string* error,
                             unsigned master_w = 0, unsigned master_h = 0,
                             const CropRect* crop = nullptr, unsigned dst_w = 0, unsigned dst_h = 0);

/** Even-normalized size of a TIFF the portable reader accepts. Same return codes as the loader. */
int probe_float_tiff_portable(const std::string& path, unsigned* master_w, unsigned* master_h,
                              std::string* error);

/** One stderr line saying why the portable reader passed on path. */
void log_float_tiff_fallback(const std::string& path);

}  // namespace uhdr_repack
