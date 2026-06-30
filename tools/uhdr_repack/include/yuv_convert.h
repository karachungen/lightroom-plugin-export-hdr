#pragma once

#include <cstdint>
#include <string>

namespace uhdr_repack {

bool rgba8888_to_yuv420_bt709(const uint8_t* rgba, unsigned w, unsigned h, uint8_t** plane_y,
                              uint8_t** plane_u, uint8_t** plane_v, std::string* error);

}  // namespace uhdr_repack
