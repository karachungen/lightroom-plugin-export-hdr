#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

/** ICC v2.1 matrix/TRC profile, linear curve, description "Linear Rec.2020". */
std::vector<uint8_t> build_linear_rec2020_icc();

/** ICC v2.1 matrix/TRC profile, 1024-entry sRGB EOTF table (device value -> linear), "Display P3". */
std::vector<uint8_t> build_display_p3_icc();

bool validate_linear_rec2020_icc(const std::vector<uint8_t>& icc, std::string* error);
bool validate_display_p3_icc(const std::vector<uint8_t>& icc, std::string* error);

enum class IccPrimaries { kUnknown, kSrgb, kDisplayP3, kRec2020 };
enum class IccTransfer { kUnknown, kLinear, kSrgb };

struct IccClass {
  IccPrimaries primaries = IccPrimaries::kUnknown;
  IccTransfer transfer = IccTransfer::kUnknown;
};

/**
 * Recognize an RGB matrix/TRC profile by its colorants and curves, not its name.
 * Profiles with an A2B0 LUT, or curves that are neither linear nor the sRGB EOTF, are kUnknown.
 */
IccClass classify_icc(const std::vector<uint8_t>& icc);

/** "Rec.2020 / linear", "Display P3 / sRGB curve", "unknown primaries / unknown curve", ... */
std::string icc_class_name(const IccClass& c);

/** ICC profile of a TIFF (tag 34675) or of a JPEG's primary image (ICC_PROFILE APP2 chunks). */
bool read_embedded_icc(const std::string& path, std::vector<uint8_t>* icc, std::string* error);

}  // namespace uhdr_repack
