#pragma once

#include "color_primaries.h"

#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

/** Display P3 linear SDR rendition of a linear Rec.2020 HDR value: clip negatives, convert, clamp to [0,1]. */
LinearRgb sdr_p3(LinearRgb rec);

/** Linear Rec.2020 RGB float TIFF, Deflate + predictor 3, embedded ICC. rgb is width*height*3.
 *  RowsPerStrip is 16; a short last strip is written when height is not a multiple of 16. */
bool write_float_tiff(const std::string& path, const std::vector<float>& rgb, int width, int height,
                      const std::vector<uint8_t>& icc, std::string* error);

/** RGBA8 (sRGB-encoded Display P3) to a q95 baseline JPEG with the given ICC. */
bool encode_p3_jpeg(const uint8_t* rgba, int width, int height, const std::vector<uint8_t>& icc,
                    std::vector<uint8_t>* jpeg, std::string* error);

bool write_bytes(const std::string& path, const std::vector<uint8_t>& bytes, std::string* error);

}  // namespace uhdr_repack
