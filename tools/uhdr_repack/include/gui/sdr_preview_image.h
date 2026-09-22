#pragma once

#include <QImage>

#include <string>

namespace uhdr_repack {

/** Pixel size of an SDR JPEG/PNG/TIFF via ImageIO or WIC. Does not use Qt's JPEG plugin. */
bool sdr_preview_size(const std::string& path, int* width, int* height, std::string* error);

/** Decode an SDR file into a QImage via ImageIO or WIC. */
bool load_sdr_preview_image(const std::string& path, QImage* out, std::string* error);

/** Print `sdr: WxH` and return 0, or 1 on failure. */
int probe_sdr_main(const std::string& path);

}  // namespace uhdr_repack
