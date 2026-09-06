#ifdef _WIN32

#include "sdr_input.h"

#include "wic_utils.h"
#include "yuv_convert.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace uhdr_repack {

bool load_sdr_base_raw_impl(const std::string& path, unsigned master_width, unsigned master_height,
                            RawImageHolder* out, std::string* error, const CropRect* crop,
                            bool color_managed) {
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
  const bool decoded = color_managed
                           ? wic::decode_scale_crop_to_rgba8_color_managed(
                                 path, master_width, master_height, out_w, out_h, crop_x, crop_y,
                                 rgba, error)
                           : wic::decode_scale_crop_to_rgba8(path, master_width, master_height,
                                                            out_w, out_h, crop_x, crop_y, rgba,
                                                            error);
  if (!decoded) {
    return false;
  }

  if (color_managed) {
    void* packed = std::malloc(rgba.size());
    if (!packed) {
      if (error) *error = "Out of memory allocating compatibility SDR buffer";
      return false;
    }
    std::memcpy(packed, rgba.data(), rgba.size());
    uhdr_raw_image_t& r = out->ref();
    std::memset(&r, 0, sizeof(r));
    r.fmt = UHDR_IMG_FMT_32bppRGBA8888;
    r.cg = UHDR_CG_BT_709;
    r.ct = UHDR_CT_SRGB;
    r.range = UHDR_CR_FULL_RANGE;
    r.w = out_w;
    r.h = out_h;
    r.planes[UHDR_PLANE_PACKED] = packed;
    r.stride[UHDR_PLANE_PACKED] = out_w;
    return true;
  }

  uint8_t* py = nullptr;
  uint8_t* pu = nullptr;
  uint8_t* pv = nullptr;
  if (!rgba8888_to_yuv420_bt709(rgba.data(), out_w, out_h, &py, &pu, &pv, error)) {
    return false;
  }

  uhdr_raw_image_t& r = out->ref();
  std::memset(&r, 0, sizeof(r));
  r.fmt = UHDR_IMG_FMT_12bppYCbCr420;
  r.cg = UHDR_CG_BT_709;
  r.ct = UHDR_CT_SRGB;
  r.range = UHDR_CR_FULL_RANGE;
  r.w = out_w;
  r.h = out_h;
  r.planes[UHDR_PLANE_Y] = py;
  r.planes[UHDR_PLANE_U] = pu;
  r.planes[UHDR_PLANE_V] = pv;
  r.stride[UHDR_PLANE_Y] = out_w;
  r.stride[UHDR_PLANE_U] = out_w / 2;
  r.stride[UHDR_PLANE_V] = out_w / 2;
  return true;
}

bool load_sdr_base_raw(const std::string& path, unsigned master_width, unsigned master_height,
                       RawImageHolder* out, std::string* error, const CropRect* crop) {
  return load_sdr_base_raw_impl(path, master_width, master_height, out, error, crop, false);
}

bool load_sdr_base_raw_compatibility(const std::string& path, unsigned master_width,
                                     unsigned master_height, RawImageHolder* out,
                                     std::string* error, const CropRect* crop) {
  return load_sdr_base_raw_impl(path, master_width, master_height, out, error, crop, true);
}

}  // namespace uhdr_repack

#endif
