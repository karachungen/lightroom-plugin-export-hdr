#ifdef _WIN32

#include "sdr_input.h"

#include "sdr_jpeg.h"
#include "wic_utils.h"

#include <cstring>
#include <vector>

namespace uhdr_repack {

bool load_sdr_base_raw(const std::string& path, unsigned master_width, unsigned master_height,
                       RawImageHolder* out, std::string* error, const CropRect* crop) {
  if (!out || master_width == 0 || master_height == 0) {
    if (error) {
      *error = "invalid arguments";
    }
    return false;
  }
  out->reset();

  unsigned out_w = master_width;
  unsigned out_h = master_height;
  unsigned crop_x = 0;
  unsigned crop_y = 0;
  if (crop) {
    if (crop->w < 2 || crop->h < 2 || (crop->w % 2) || (crop->h % 2)) {
      if (error) {
        *error = "SDR crop must have even width and height >= 2";
      }
      return false;
    }
    if (crop->x + crop->w > master_width || crop->y + crop->h > master_height) {
      if (error) {
        *error = "SDR crop rect exceeds master bounds";
      }
      return false;
    }
    out_w = crop->w;
    out_h = crop->h;
    crop_x = crop->x;
    crop_y = crop->y;
  }

  std::vector<uint8_t> rgba;
  const int portable = load_sdr_jpeg_p3(path, master_width, master_height, out_w, out_h, crop_x, crop_y,
                                        &rgba, error);
  if (portable < 0) return false;
  if (portable > 0) return rgba_p3_to_sdr_raw(rgba, out_w, out_h, out, error);
  log_sdr_jpeg_fallback(path);
  if (!wic::decode_scale_crop_to_rgba8(path, master_width, master_height, out_w, out_h, crop_x,
                                       crop_y, rgba, error, wic::Rgba8Space::DisplayP3)) {
    return false;
  }
  return rgba_p3_to_sdr_raw(rgba, out_w, out_h, out, error);
}

}  // namespace uhdr_repack

#endif
