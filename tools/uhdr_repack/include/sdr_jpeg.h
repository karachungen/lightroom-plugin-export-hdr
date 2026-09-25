#pragma once

#include "icc_profile.h"
#include "raw_image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

/** What the portable SDR loader saw. supported=false means an OS decoder reads the file. */
struct SdrJpegInfo {
  bool is_jpeg = false;
  bool supported = false;
  std::string why;
  unsigned width = 0;
  unsigned height = 0;
  bool has_icc = false;
  IccClass icc;
};

bool describe_sdr_jpeg(const std::string& path, SdrJpegInfo* info, std::string* error);

/** libjpeg decode to RGBA8 (A = 255). */
bool decode_jpeg_rgba8(const std::vector<uint8_t>& jpeg, std::vector<uint8_t>& rgba, unsigned* width,
                       unsigned* height, std::string* error);

/** Nearest-neighbor scale of src to master size, then crop out_w×out_h at crop_x, crop_y. */
bool scale_crop_rgba8_buffer(const std::vector<uint8_t>& src, unsigned src_w, unsigned src_h,
                             unsigned master_w, unsigned master_h, unsigned out_w, unsigned out_h,
                             unsigned crop_x, unsigned crop_y, std::vector<uint8_t>& dst,
                             std::string* error);

/**
 * Decode an sRGB-curve JPEG (untagged = sRGB, or tagged sRGB or Display P3 primaries) into
 * Display P3 RGBA8, scaled to master size and cropped. Same code on macOS and Windows.
 * @return 1 loaded, 0 not handled here (caller uses the OS decoder), -1 error.
 */
int load_sdr_jpeg_p3(const std::string& path, unsigned master_w, unsigned master_h, unsigned out_w,
                     unsigned out_h, unsigned crop_x, unsigned crop_y, std::vector<uint8_t>* rgba,
                     std::string* error);

/** Display P3 RGBA8 to a BT.601 4:2:0 libultrahdr image tagged Display P3, sRGB transfer, full range. */
bool rgba_p3_to_sdr_raw(const std::vector<uint8_t>& rgba, unsigned w, unsigned h, RawImageHolder* out,
                        std::string* error);

/** One stderr line saying why the portable loader passed on path. */
void log_sdr_jpeg_fallback(const std::string& path);

}  // namespace uhdr_repack
