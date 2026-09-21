#pragma once

#include "raw_image.h"

#include <string>

namespace uhdr_repack {

/** Lanczos-3 resize of linear Rec.2020 RGBA half-float HDR. */
bool resize_hdr_lanczos(const RawImageHolder& src, unsigned dst_w, unsigned dst_h,
                        RawImageHolder* dst, std::string* error);

/** Linear-light Lanczos-3 of Display P3 SDR 4:2:0 (BT.601), then sRGB-transfer unsharp mask. */
bool resize_sdr_lanczos_sharpen(const RawImageHolder& src, unsigned dst_w, unsigned dst_h,
                                RawImageHolder* dst, std::string* error);

}  // namespace uhdr_repack
