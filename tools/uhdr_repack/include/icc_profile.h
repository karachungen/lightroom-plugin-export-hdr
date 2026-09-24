#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

/** ICC v2.1 matrix/TRC profile, linear curve, description "Linear Rec.2020". */
std::vector<uint8_t> build_linear_rec2020_icc();

/** ICC v2.1 matrix/TRC profile, sRGB curve table, description "Display P3". */
std::vector<uint8_t> build_display_p3_icc();

bool validate_linear_rec2020_icc(const std::vector<uint8_t>& icc, std::string* error);
bool validate_display_p3_icc(const std::vector<uint8_t>& icc, std::string* error);

/** OS CMM accepts the profile bytes (WIC on Windows, CoreGraphics on macOS). */
bool icc_accepted_by_os(const uint8_t* data, std::size_t size, std::string* error);

/**
 * Open an image and require an embedded profile whose description contains
 * description_substring. Fails when the file or the profile cannot be read.
 */
bool image_has_color_profile(const std::string& path, const char* description_substring,
                             std::string* error);

/**
 * Decode a TIFF to tightly packed float RGB (no alpha).
 * Windows returns WIC's stored float samples. macOS renders through Core Image
 * into extended linear Rec.2020.
 */
bool read_tiff_float_rgb_os(const std::string& path, std::vector<float>* rgb, int* width, int* height,
                            std::string* error);

}  // namespace uhdr_repack
