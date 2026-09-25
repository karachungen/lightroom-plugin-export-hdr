#pragma once

#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace uhdr_repack {

/** Editor SDR preview long-edge cap. Encode still reads the file at full size. */
constexpr int kSdrPreviewMaxEdge = 4096;

/**
 * Fit src into max_edge on the long side. max_edge < 2, or a source that already fits,
 * returns the source size. Does not require even dimensions.
 */
inline bool fit_preview_long_edge(int src_w, int src_h, int max_edge, int* dst_w, int* dst_h) {
  if (!dst_w || !dst_h || src_w < 1 || src_h < 1) return false;
  if (max_edge < 2 || (src_w <= max_edge && src_h <= max_edge)) {
    *dst_w = src_w;
    *dst_h = src_h;
    return true;
  }
  const double scale =
      static_cast<double>(max_edge) / static_cast<double>(std::max(src_w, src_h));
  int w = std::max(1, static_cast<int>(std::lround(static_cast<double>(src_w) * scale)));
  int h = std::max(1, static_cast<int>(std::lround(static_cast<double>(src_h) * scale)));
  if (std::max(w, h) > max_edge) {
    if (w >= h) {
      h = std::max(1, static_cast<int>(std::lround(static_cast<double>(src_h) * scale)));
      w = max_edge;
      if (h > max_edge) h = max_edge;
    } else {
      w = std::max(1, static_cast<int>(std::lround(static_cast<double>(src_w) * scale)));
      h = max_edge;
      if (w > max_edge) w = max_edge;
    }
  }
  *dst_w = w;
  *dst_h = h;
  return true;
}

/** Pixel size of an SDR JPEG/PNG/TIFF via ImageIO or WIC. Does not use Qt's JPEG plugin. */
bool sdr_preview_size(const std::string& path, int* width, int* height, std::string* error);

/**
 * Decode an SDR file into a QImage via ImageIO or WIC.
 * max_edge <= 0 decodes the full frame. A positive max_edge caps the long side for preview.
 */
bool load_sdr_preview_image(const std::string& path, QImage* out, std::string* error,
                            int max_edge = 0);

/** Encode a QImage as JPEG bytes via ImageIO or WIC. quality is 1–100. */
bool encode_preview_jpeg(const QImage& image, int quality, std::vector<uint8_t>* out,
                         std::string* error);

/** Print `sdr: WxH` and return 0, or 1 on failure. */
int probe_sdr_main(const std::string& path);

/** Load path, encode a JPEG data URL, print one line, return 0 or 1. */
int encode_preview_jpeg_main(const std::string& path);

/** Encode a preview JPEG, check the SOI marker, reload it; return 0 or 1. */
int check_preview_jpeg_main(const std::string& path);

}  // namespace uhdr_repack
