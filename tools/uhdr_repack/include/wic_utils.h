#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

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

/** Color-managed 8-bit RGBA. Display P3 uses the sRGB transfer (Ultra HDR primary). sRGB is for the web preview. */
enum class Rgba8Space { DisplayP3, Srgb };

bool decode_scale_crop_to_rgba8(const std::string& path, unsigned master_width,
                                unsigned master_height, unsigned out_w, unsigned out_h,
                                unsigned crop_x, unsigned crop_y, std::vector<uint8_t>& rgba,
                                std::string* error, Rgba8Space space = Rgba8Space::DisplayP3);

/** Color-managed linear Display P3 float RGBA (A=1), after scale/crop to master pixels. */
bool decode_scale_crop_to_linear_p3(const std::string& path, unsigned master_width,
                                    unsigned master_height, unsigned out_w, unsigned out_h,
                                    unsigned crop_x, unsigned crop_y, std::vector<float>& rgba,
                                    std::string* error);

/** Full-frame color-managed 8-bit sRGB RGBA for the editor canvas. */
bool decode_to_srgb_rgba8(const std::string& path, std::vector<uint8_t>& rgba, unsigned* width,
                          unsigned* height, std::string* error);

/** libjpeg RGB JPEG (no WIC). Used for fixtures and as the Windows SDR ingest decoder. */
bool encode_jpeg_from_rgba8(const uint8_t* rgba, unsigned width, unsigned height, int quality,
                            std::vector<uint8_t>* jpeg, std::string* error);

}  // namespace wic
}  // namespace uhdr_repack

#endif
