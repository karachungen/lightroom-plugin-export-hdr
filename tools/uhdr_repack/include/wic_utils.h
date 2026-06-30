#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

namespace uhdr_repack {
namespace wic {

Microsoft::WRL::ComPtr<IWICImagingFactory> create_factory();

std::wstring utf8_to_wide(const std::string& path);

bool decode_to_rgba_float(const std::string& path, std::vector<float>& rgba, unsigned* width,
                          unsigned* height, std::string* error);

bool decode_scale_crop_to_rgba8(const std::string& path, unsigned master_width,
                                unsigned master_height, unsigned out_w, unsigned out_h,
                                unsigned crop_x, unsigned crop_y, std::vector<uint8_t>& rgba,
                                std::string* error);

}  // namespace wic
}  // namespace uhdr_repack

#endif
