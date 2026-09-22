#pragma once

#include <QImage>

#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

/** Pixel size of an SDR JPEG/PNG/TIFF via ImageIO or WIC. Does not use Qt's JPEG plugin. */
bool sdr_preview_size(const std::string& path, int* width, int* height, std::string* error);

/** Decode an SDR file into a QImage via ImageIO or WIC. */
bool load_sdr_preview_image(const std::string& path, QImage* out, std::string* error);

/** Encode a QImage as JPEG bytes via ImageIO or WIC. quality is 1–100. */
bool encode_preview_jpeg(const QImage& image, int quality, std::vector<uint8_t>* out,
                         std::string* error);

/** Print `sdr: WxH` and return 0, or 1 on failure. */
int probe_sdr_main(const std::string& path);

/** Load path, encode a JPEG data URL, print one line, return 0 or 1. */
int encode_preview_jpeg_main(const std::string& path);

}  // namespace uhdr_repack
