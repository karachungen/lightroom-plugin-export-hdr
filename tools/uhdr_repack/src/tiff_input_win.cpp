#ifdef _WIN32

#include "tiff_input.h"

#include "half_float.h"
#include "wic_utils.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

namespace uhdr_repack {

namespace {

bool load_hdr_rgba_float(const std::string& path, std::vector<float>& rgba, unsigned* width,
                         unsigned* height, std::string* error) {
  return wic::decode_to_rgba_float(path, rgba, width, height, error);
}

}  // namespace

bool probe_hdr_tiff_even_size(const std::string& path, unsigned* master_w, unsigned* master_h,
                              std::string* error) {
  if (!master_w || !master_h) {
    if (error) {
      *error = "internal: null size output";
    }
    return false;
  }

  std::vector<float> rgba;
  unsigned w = 0;
  unsigned h = 0;
  if (!load_hdr_rgba_float(path, rgba, &w, &h, error)) {
    return false;
  }

  unsigned norm_w = 0;
  unsigned norm_h = 0;
  even_normalize(w, h, &norm_w, &norm_h);
  if (norm_w < 2 || norm_h < 2) {
    if (error) {
      *error = "Invalid HDR image extent";
    }
    return false;
  }
  *master_w = norm_w;
  *master_h = norm_h;
  return true;
}

bool load_hdr_tiff_raw(const std::string& path, RawImageHolder* out, std::string* error,
                       unsigned master_w, unsigned master_h, const CropRect* crop) {
  if (!out) {
    if (error) {
      *error = "internal: null output";
    }
    return false;
  }
  out->reset();

  std::vector<float> rgba;
  unsigned orig_w = 0;
  unsigned orig_h = 0;
  if (!load_hdr_rgba_float(path, rgba, &orig_w, &orig_h, error)) {
    return false;
  }

  unsigned norm_w = 0;
  unsigned norm_h = 0;
  even_normalize(orig_w, orig_h, &norm_w, &norm_h);
  if (norm_w < 2 || norm_h < 2) {
    if (error) {
      *error = "Invalid image extent";
    }
    return false;
  }

  if (crop) {
    if (master_w == 0 || master_h == 0) {
      if (error) {
        *error = "Master dimensions required when cropping HDR TIFF";
      }
      return false;
    }
    if (master_w != norm_w || master_h != norm_h) {
      if (error) {
        *error = "HDR master size mismatch for crop";
      }
      return false;
    }
    if (crop->w < 2 || crop->h < 2 || (crop->w % 2) || (crop->h % 2)) {
      if (error) {
        *error = "HDR crop must have even width and height >= 2";
      }
      return false;
    }
    if (crop->x + crop->w > norm_w || crop->y + crop->h > norm_h) {
      if (error) {
        *error = "HDR crop rect exceeds image bounds";
      }
      return false;
    }
  } else if (norm_w != orig_w || norm_h != orig_h) {
    std::fprintf(stderr, "HDR dimensions cropped from %ux%u to %ux%u for 4:2:0 compatibility\n",
                 orig_w, orig_h, norm_w, norm_h);
  }

  const unsigned w = crop ? crop->w : norm_w;
  const unsigned h = crop ? crop->h : norm_h;
  const unsigned ox = crop ? crop->x : 0;
  const unsigned oy = crop ? crop->y : 0;

  std::vector<float> crop_buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      const unsigned src_x = ox + x;
      const unsigned src_y = oy + y;
      const size_t src_i = (static_cast<size_t>(src_y) * orig_w + src_x) * 4;
      const size_t dst_i = (static_cast<size_t>(y) * w + x) * 4;
      crop_buf[dst_i] = rgba[src_i];
      crop_buf[dst_i + 1] = rgba[src_i + 1];
      crop_buf[dst_i + 2] = rgba[src_i + 2];
      crop_buf[dst_i + 3] = rgba[src_i + 3];
    }
  }

  const size_t hcount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
  void* hdata = std::malloc(hcount * sizeof(fp16_t));
  if (!hdata) {
    if (error) {
      *error = "Out of memory allocating HDR buffer";
    }
    return false;
  }
  float_rgba_to_half_rgba(crop_buf.data(), reinterpret_cast<fp16_t*>(hdata), hcount);

  uhdr_raw_image_t& r = out->ref();
  std::memset(&r, 0, sizeof(r));
  r.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
  r.cg = UHDR_CG_BT_2100;
  r.ct = UHDR_CT_LINEAR;
  r.range = UHDR_CR_FULL_RANGE;
  r.w = w;
  r.h = h;
  r.planes[UHDR_PLANE_PACKED] = hdata;
  r.stride[UHDR_PLANE_PACKED] = w;
  return true;
}

}  // namespace uhdr_repack

#endif
