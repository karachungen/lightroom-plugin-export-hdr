#include "tiff_float.h"

#include "half_float.h"
#include "path_io.h"
#include "resize_lanczos.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace uhdr_repack {

namespace {

uint16_t tiff_u16(const uint8_t* p, bool le) {
  return le ? static_cast<uint16_t>(p[0] | (p[1] << 8))
            : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t tiff_u32(const uint8_t* p, bool le) {
  return le ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)
            : ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

const uint8_t* tiff_values(const std::vector<uint8_t>& file, const uint8_t* entry, bool le,
                           uint16_t type, uint32_t count) {
  if (type == 3) {
    if (count > 2) {
      const uint32_t off = tiff_u32(entry + 8, le);
      if (static_cast<size_t>(off) + static_cast<size_t>(count) * 2u > file.size()) return nullptr;
      return file.data() + off;
    }
    return entry + 8;
  }
  if (type == 4) {
    if (count > 1) {
      const uint32_t off = tiff_u32(entry + 8, le);
      if (static_cast<size_t>(off) + static_cast<size_t>(count) * 4u > file.size()) return nullptr;
      return file.data() + off;
    }
    return entry + 8;
  }
  if (type == 7) {
    const uint32_t off = count <= 4 ? 0 : tiff_u32(entry + 8, le);
    if (count <= 4) return entry + 8;
    if (static_cast<size_t>(off) + count > file.size()) return nullptr;
    return file.data() + off;
  }
  return nullptr;
}

uint32_t tiff_scalar(const uint8_t* entry, bool le) {
  const uint16_t type = tiff_u16(entry + 2, le);
  if (type == 3) return tiff_u16(entry + 8, le);
  return tiff_u32(entry + 8, le);
}

struct RawFloatTiff {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<float> rgb;
};

enum class FloatRead { kFallback, kRaw, kError };

FloatRead read_identity_rec2020_float(const std::string& path, bool load_pixels, RawFloatTiff* out,
                                      std::string* error) {
  std::ifstream input = open_input_binary(path);
  if (!input) {
    if (error) *error = "Could not open HDR file: " + path;
    return FloatRead::kError;
  }
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (file.size() < 8) return FloatRead::kFallback;
  bool le = false;
  if (file[0] == 'I' && file[1] == 'I') {
    le = true;
  } else if (file[0] == 'M' && file[1] == 'M') {
    le = false;
  } else {
    return FloatRead::kFallback;
  }
  if (tiff_u16(file.data() + 2, le) != 42) return FloatRead::kFallback;
  const uint32_t ifd = tiff_u32(file.data() + 4, le);
  if (static_cast<size_t>(ifd) + 2 > file.size()) return FloatRead::kFallback;
  const uint16_t ntags = tiff_u16(file.data() + ifd, le);
  if (static_cast<size_t>(ifd) + 2u + static_cast<size_t>(ntags) * 12u > file.size()) {
    return FloatRead::kFallback;
  }

  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t compression = 1;
  uint16_t photometric = 2;
  uint16_t samples = 1;
  uint16_t planar = 1;
  uint16_t orientation = 1;
  uint32_t rows_per_strip = 0;
  bool has_icc = false;
  bool linear_rec2020 = false;
  const uint8_t* bits = nullptr;
  uint32_t bits_count = 0;
  const uint8_t* sample_fmt = nullptr;
  uint32_t sample_fmt_count = 0;
  const uint8_t* strip_off = nullptr;
  uint32_t strip_off_count = 0;
  uint16_t strip_off_type = 4;
  const uint8_t* strip_bytes = nullptr;
  uint32_t strip_bytes_count = 0;
  uint16_t strip_bytes_type = 4;
  bool tiled = false;

  for (uint16_t i = 0; i < ntags; ++i) {
    const uint8_t* entry = file.data() + ifd + 2 + static_cast<size_t>(i) * 12u;
    const uint16_t tag = tiff_u16(entry, le);
    const uint16_t type = tiff_u16(entry + 2, le);
    const uint32_t count = tiff_u32(entry + 4, le);
    if (tag == 256) width = tiff_scalar(entry, le);
    else if (tag == 257) height = tiff_scalar(entry, le);
    else if (tag == 258) {
      bits = tiff_values(file, entry, le, type, count);
      bits_count = count;
    } else if (tag == 259) compression = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 262) photometric = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 273 || tag == 324) {
      if (tag == 324) tiled = true;
      strip_off = tiff_values(file, entry, le, type, count);
      strip_off_count = count;
      strip_off_type = type;
    } else if (tag == 274) orientation = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 277) samples = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 278 || tag == 323) {
      if (tag == 323) tiled = true;
      rows_per_strip = tiff_scalar(entry, le);
    } else if (tag == 279 || tag == 325) {
      if (tag == 325) tiled = true;
      strip_bytes = tiff_values(file, entry, le, type, count);
      strip_bytes_count = count;
      strip_bytes_type = type;
    } else if (tag == 284) planar = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 322) tiled = true;
    else if (tag == 339) {
      sample_fmt = tiff_values(file, entry, le, type, count);
      sample_fmt_count = count;
    } else if (tag == 34675 && count > 4) {
      has_icc = true;
      const uint8_t* icc = tiff_values(file, entry, le, 7, count);
      if (icc) {
        const std::string hay(reinterpret_cast<const char*>(icc), count);
        linear_rec2020 = hay.find("Linear Rec.2020") != std::string::npos;
      }
    }
  }

  const bool float32 = bits && bits_count >= 3 && tiff_u16(bits, le) == 32 &&
                       tiff_u16(bits + 2, le) == 32 && tiff_u16(bits + 4, le) == 32;
  const bool ieee = sample_fmt && sample_fmt_count >= 3 && tiff_u16(sample_fmt, le) == 3 &&
                    tiff_u16(sample_fmt + 2, le) == 3 && tiff_u16(sample_fmt + 4, le) == 3;
  const bool identity = !tiled && compression == 1 && photometric == 2 && samples == 3 &&
                        planar == 1 && orientation <= 1 && float32 && ieee &&
                        (!has_icc || linear_rec2020);
  if (!identity || width < 2 || height < 2 || !strip_off || strip_off_count < 1) {
    return FloatRead::kFallback;
  }

  out->width = width;
  out->height = height;
  if (!load_pixels) return FloatRead::kRaw;
  if (rows_per_strip == 0) rows_per_strip = height;

  out->rgb.assign(static_cast<size_t>(width) * height * 3u, 0.f);
  uint32_t row = 0;
  for (uint32_t s = 0; s < strip_off_count && row < height; ++s) {
    const uint32_t off = strip_off_type == 3 ? tiff_u16(strip_off + s * 2u, le)
                                             : tiff_u32(strip_off + s * 4u, le);
    const uint32_t nbytes = (strip_bytes && s < strip_bytes_count)
                                ? (strip_bytes_type == 3 ? tiff_u16(strip_bytes + s * 2u, le)
                                                         : tiff_u32(strip_bytes + s * 4u, le))
                                : rows_per_strip * width * 12u;
    const uint32_t rows = std::min(rows_per_strip, height - row);
    const size_t need = static_cast<size_t>(rows) * width * 12u;
    if (static_cast<size_t>(off) + need > file.size() || nbytes < need) {
      if (error) *error = "HDR TIFF float strips are truncated: " + path;
      return FloatRead::kError;
    }
    std::memcpy(out->rgb.data() + static_cast<size_t>(row) * width * 3u, file.data() + off, need);
    row += rows;
  }
  if (row < height) {
    if (error) *error = "HDR TIFF float strips do not cover the image: " + path;
    return FloatRead::kError;
  }
  return FloatRead::kRaw;
}

bool publish_float_rgb(const std::vector<float>& rgb, unsigned src_w, unsigned src_h, unsigned w,
                       unsigned h, unsigned ox, unsigned oy, unsigned dst_w, unsigned dst_h,
                       RawImageHolder* out, std::string* error) {
  std::vector<float> rgba(static_cast<size_t>(w) * h * 4u, 1.f);
  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      const size_t s = (static_cast<size_t>(oy + y) * src_w + (ox + x)) * 3u;
      const size_t d = (static_cast<size_t>(y) * w + x) * 4u;
      rgba[d] = rgb[s];
      rgba[d + 1] = rgb[s + 1];
      rgba[d + 2] = rgb[s + 2];
      rgba[d + 3] = 1.f;
    }
  }
  const size_t hcount = static_cast<size_t>(w) * h * 4u;
  void* hdata = std::malloc(hcount * sizeof(fp16_t));
  if (!hdata) {
    if (error) *error = "Out of memory allocating HDR buffer";
    return false;
  }
  float_rgba_to_half_rgba(rgba.data(), reinterpret_cast<fp16_t*>(hdata), hcount);

  uhdr_raw_image_t& image = out->ref();
  std::memset(&image, 0, sizeof(image));
  image.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
  image.cg = UHDR_CG_BT_2100;
  image.ct = UHDR_CT_LINEAR;
  image.range = UHDR_CR_FULL_RANGE;
  image.w = w;
  image.h = h;
  image.planes[UHDR_PLANE_PACKED] = hdata;
  image.stride[UHDR_PLANE_PACKED] = w;

  if (dst_w >= 2 && dst_h >= 2 && (dst_w != w || dst_h != h)) {
    if (dst_w % 2) --dst_w;
    if (dst_h % 2) --dst_h;
    RawImageHolder scaled;
    if (!resize_hdr_lanczos(*out, dst_w, dst_h, &scaled, error)) return false;
    *out = std::move(scaled);
  }
  return true;
}

bool publish_raw(const RawFloatTiff& raw, RawImageHolder* out, std::string* error, unsigned master_w,
                 unsigned master_h, const CropRect* crop, unsigned dst_w, unsigned dst_h) {
  unsigned norm_w = 0;
  unsigned norm_h = 0;
  even_normalize(raw.width, raw.height, &norm_w, &norm_h);
  if (norm_w < 2 || norm_h < 2) {
    if (error) *error = "Invalid image extent";
    return false;
  }
  if (crop) {
    if (master_w == 0 || master_h == 0) {
      if (error) *error = "Master dimensions required when cropping HDR TIFF";
      return false;
    }
    if (master_w != norm_w || master_h != norm_h) {
      if (error) *error = "HDR master size mismatch for crop";
      return false;
    }
    if (crop->w < 2 || crop->h < 2 || (crop->w % 2) || (crop->h % 2)) {
      if (error) *error = "HDR crop must have even width and height >= 2";
      return false;
    }
    if (crop->x + crop->w > norm_w || crop->y + crop->h > norm_h) {
      if (error) *error = "HDR crop rect exceeds image bounds";
      return false;
    }
  } else if (norm_w != raw.width || norm_h != raw.height) {
    std::fprintf(stderr, "HDR dimensions cropped from %ux%u to %ux%u for 4:2:0 compatibility\n",
                 raw.width, raw.height, norm_w, norm_h);
  }
  const unsigned w = crop ? crop->w : norm_w;
  const unsigned h = crop ? crop->h : norm_h;
  const unsigned ox = crop ? crop->x : 0;
  const unsigned oy = crop ? crop->y : 0;
  return publish_float_rgb(raw.rgb, raw.width, raw.height, w, h, ox, oy, dst_w, dst_h, out, error);
}

}  // namespace

int probe_identity_rec2020_tiff(const std::string& path, unsigned* master_w, unsigned* master_h,
                                std::string* error) {
  if (!master_w || !master_h) {
    if (error) *error = "internal: null size output";
    return -1;
  }
  RawFloatTiff raw;
  const FloatRead kind = read_identity_rec2020_float(path, false, &raw, error);
  if (kind == FloatRead::kError) return -1;
  if (kind == FloatRead::kFallback) return 0;
  unsigned w = 0;
  unsigned h = 0;
  even_normalize(raw.width, raw.height, &w, &h);
  if (w < 2 || h < 2) {
    if (error) *error = "Invalid HDR image extent";
    return -1;
  }
  *master_w = w;
  *master_h = h;
  return 1;
}

int load_identity_rec2020_tiff(const std::string& path, RawImageHolder* out, std::string* error,
                               unsigned master_w, unsigned master_h, const CropRect* crop,
                               unsigned dst_w, unsigned dst_h) {
  if (!out) {
    if (error) *error = "internal: null output";
    return -1;
  }
  RawFloatTiff raw;
  const FloatRead kind = read_identity_rec2020_float(path, true, &raw, error);
  if (kind == FloatRead::kError) return -1;
  if (kind == FloatRead::kFallback) return 0;
  out->reset();
  if (!publish_raw(raw, out, error, master_w, master_h, crop, dst_w, dst_h)) return -1;
  return 1;
}

}  // namespace uhdr_repack
