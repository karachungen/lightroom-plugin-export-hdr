#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

inline constexpr char kXmpRightsWebStatement[] = "https://hdr.karachun.by/";
inline constexpr char kXmpRightsUsageTerms[] =
    "https://github.com/karachungen/lightroom-plugin-export-hdr";

/** Merge xmpRights into the primary (SDR) APP1 XMP packet. Gain-map XMP is left unchanged. */
bool inject_sdr_xmp_rights(std::vector<uint8_t>* jpeg, std::string* error);

}  // namespace uhdr_repack
