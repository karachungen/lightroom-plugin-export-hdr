#pragma once

#include "raw_image.h"

#include <string>
#include <vector>

namespace uhdr_repack {

/** Load float32 gain map from .gainmap file (header: 2x int32 w,h then w*h floats). */
bool load_gainmap_raw_file(const std::string& path, std::vector<float>* out, int* width, int* height,
                           std::string* error);

/** Inject external gain map into encoder before encode (replaces auto-computed map). */
bool apply_external_gainmap(uhdr_codec_private_t* enc, const std::string& path,
                            unsigned expected_w, unsigned expected_h, std::string* error);

}  // namespace uhdr_repack
