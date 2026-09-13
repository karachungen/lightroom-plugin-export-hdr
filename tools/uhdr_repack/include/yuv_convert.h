#pragma once

#include <cstdint>
#include <string>

namespace uhdr_repack {

bool rgba8888_to_yuv420_bt709(const uint8_t* rgba, unsigned w, unsigned h, uint8_t** plane_y,
                              uint8_t** plane_u, uint8_t** plane_v, std::string* error);

bool rgba8888_to_yuv420_bt601(const uint8_t* rgba, unsigned w, unsigned h, uint8_t** plane_y,
                              uint8_t** plane_u, uint8_t** plane_v, std::string* error);

bool yuv420_bt709_to_rgba8888(const uint8_t* plane_y, const uint8_t* plane_u, const uint8_t* plane_v,
                              unsigned w, unsigned h, unsigned stride_y, unsigned stride_u,
                              unsigned stride_v, uint8_t* rgba, std::string* error);

bool yuv420_bt601_to_rgba8888(const uint8_t* plane_y, const uint8_t* plane_u, const uint8_t* plane_v,
                              unsigned w, unsigned h, unsigned stride_y, unsigned stride_u,
                              unsigned stride_v, uint8_t* rgba, std::string* error);

}  // namespace uhdr_repack
