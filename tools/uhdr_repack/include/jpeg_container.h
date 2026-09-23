#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

bool load_binary_file(const std::string& path, std::vector<uint8_t>* out, std::string* error);

/** JPEG SOI plus SOF0–SOF3 dimensions. */
bool probe_jpeg_sof_size(const std::vector<uint8_t>& jpeg, unsigned* width, unsigned* height);
bool probe_jpeg_sof_size(const std::string& path, unsigned* width, unsigned* height,
                         std::string* error);

/** Byte range of the first SOS through the first EOI (inclusive). */
bool jpeg_first_scan_range(const std::vector<uint8_t>& jpeg, size_t* sos, size_t* eoi);

}  // namespace uhdr_repack
