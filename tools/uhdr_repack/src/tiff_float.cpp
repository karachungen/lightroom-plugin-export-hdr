#include "tiff_float.h"

#include "color_primaries.h"
#include "half_float.h"
#include "path_io.h"
#include "resize_lanczos.h"

#include <miniz.h>

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

FloatRead pass_on(FloatTiffInfo* info, const char* why) {
  info->supported = false;
  info->why = why;
  return FloatRead::kFallback;
}

uint32_t tiff_at(const uint8_t* p, uint16_t type, uint32_t i, bool le) {
  return type == 3 ? tiff_u16(p + i * 2u, le) : tiff_u32(p + i * 4u, le);
}

void undo_float_predictor(uint8_t* row, size_t values, uint32_t stride) {
  const size_t bytes = values * 4u;
  for (size_t i = stride; i < bytes; ++i) row[i] = static_cast<uint8_t>(row[i] + row[i - stride]);
  const std::vector<uint8_t> planes(row, row + bytes);
  for (size_t v = 0; v < values; ++v) {
    for (size_t b = 0; b < 4; ++b) row[v * 4u + b] = planes[(3 - b) * values + v];
  }
}

void swap_float_bytes(uint8_t* p, size_t values) {
  for (size_t v = 0; v < values; ++v, p += 4) {
    std::swap(p[0], p[3]);
    std::swap(p[1], p[2]);
  }
}

FloatRead read_float_tiff(const std::string& path, bool load_pixels, RawFloatTiff* out, FloatTiffInfo* info,
                          std::string* error) {
  *info = FloatTiffInfo{};
  std::ifstream input = open_input_binary(path);
  if (!input) {
    if (error) *error = "Could not open HDR file: " + path;
    return FloatRead::kError;
  }
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (file.size() < 8) return pass_on(info, "file is too small for a TIFF");
  bool le = false;
  if (file[0] == 'I' && file[1] == 'I') le = true;
  else if (!(file[0] == 'M' && file[1] == 'M')) return pass_on(info, "not a TIFF");
  if (tiff_u16(file.data() + 2, le) != 42) return pass_on(info, "not a classic TIFF");
  info->is_tiff = true;
  info->big_endian = !le;
  const uint32_t ifd = tiff_u32(file.data() + 4, le);
  if (static_cast<size_t>(ifd) + 2 > file.size()) return pass_on(info, "TIFF IFD is past the end of the file");
  const uint16_t ntags = tiff_u16(file.data() + ifd, le);
  if (static_cast<size_t>(ifd) + 2u + static_cast<size_t>(ntags) * 12u > file.size()) {
    return pass_on(info, "TIFF IFD is truncated");
  }

  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t compression = 1;
  uint16_t predictor = 1;
  uint16_t photometric = 2;
  uint16_t samples = 1;
  uint16_t planar = 1;
  uint16_t orientation = 1;
  uint32_t rows_per_strip = 0;
  bool icc_broken = false;
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
    else if (tag == 317) predictor = static_cast<uint16_t>(tiff_scalar(entry, le));
    else if (tag == 322) tiled = true;
    else if (tag == 339) {
      sample_fmt = tiff_values(file, entry, le, type, count);
      sample_fmt_count = count;
    } else if (tag == 34675 && count > 4) {
      const uint8_t* icc = tiff_values(file, entry, le, 7, count);
      if (!icc) {
        icc_broken = true;
      } else {
        info->has_icc = true;
        info->icc = classify_icc(std::vector<uint8_t>(icc, icc + count));
      }
    }
  }
  info->width = width;
  info->height = height;
  info->compression = compression;
  info->predictor = predictor;

  auto every = [&](const uint8_t* p, uint32_t n, uint16_t want) {
    if (!p || n < samples) return false;
    for (uint32_t s = 0; s < samples; ++s) {
      if (tiff_u16(p + s * 2u, le) != want) return false;
    }
    return true;
  };
  if (tiled) return pass_on(info, "tiled TIFF");
  if (photometric != 2) return pass_on(info, "not RGB");
  if (samples != 3 && samples != 4) return pass_on(info, "not 3 or 4 samples per pixel");
  if (planar != 1) return pass_on(info, "planar TIFF");
  if (orientation > 1) return pass_on(info, "Orientation tag rotates the image");
  if (!every(bits, bits_count, 32) || !every(sample_fmt, sample_fmt_count, 3)) {
    return pass_on(info, "not 32-bit IEEE float samples");
  }
  if (compression != 1 && compression != 8 && compression != 32946) {
    return pass_on(info, "compression is not none or Deflate");
  }
  if (predictor != 1 && predictor != 3) return pass_on(info, "predictor is not 1 or 3");
  if (icc_broken) return pass_on(info, "ICC tag is past the end of the file");
  if (info->has_icc && info->icc.transfer != IccTransfer::kLinear) return pass_on(info, "ICC curve is not linear");
  if (info->has_icc && info->icc.primaries == IccPrimaries::kUnknown) {
    return pass_on(info, "ICC primaries are not Rec.2020, Display P3, or sRGB");
  }
  if (width < 2 || height < 2 || !strip_off || strip_off_count < 1) return pass_on(info, "missing size or strips");
  if (compression != 1 && (!strip_bytes || strip_bytes_count < strip_off_count)) {
    return pass_on(info, "compressed TIFF has no strip byte counts");
  }
  info->supported = true;

  out->width = width;
  out->height = height;
  if (!load_pixels) return FloatRead::kRaw;
  if (rows_per_strip == 0 || rows_per_strip > height) rows_per_strip = height;

  const size_t row_values = static_cast<size_t>(width) * samples;
  const size_t row_bytes = row_values * 4u;
  out->rgb.assign(static_cast<size_t>(width) * height * 3u, 0.f);
  std::vector<uint8_t> strip;
  uint32_t row = 0;
  for (uint32_t s = 0; s < strip_off_count && row < height; ++s) {
    const uint32_t off = tiff_at(strip_off, strip_off_type, s, le);
    const uint32_t rows = std::min(rows_per_strip, height - row);
    const size_t need = static_cast<size_t>(rows) * row_bytes;
    const size_t nbytes = (strip_bytes && s < strip_bytes_count) ? tiff_at(strip_bytes, strip_bytes_type, s, le) : need;
    if (static_cast<size_t>(off) + nbytes > file.size() || (compression == 1 && nbytes < need)) {
      if (error) *error = "HDR TIFF strips are truncated: " + path;
      return FloatRead::kError;
    }
    if (compression == 1) {
      strip.assign(file.begin() + off, file.begin() + off + need);
    } else {
      strip.assign(need, 0);
      mz_ulong len = static_cast<mz_ulong>(need);
      if (mz_uncompress(strip.data(), &len, file.data() + off, static_cast<mz_ulong>(nbytes)) != MZ_OK ||
          len != need) {
        if (error) *error = "HDR TIFF Deflate strip is corrupt: " + path;
        return FloatRead::kError;
      }
    }
    for (uint32_t r = 0; r < rows; ++r) {
      uint8_t* p = strip.data() + static_cast<size_t>(r) * row_bytes;
      if (predictor == 3) undo_float_predictor(p, row_values, samples);
      else if (!le) swap_float_bytes(p, row_values);
      float* dst = out->rgb.data() + static_cast<size_t>(row + r) * width * 3u;
      for (uint32_t x = 0; x < width; ++x) {
        std::memcpy(dst + static_cast<size_t>(x) * 3u, p + static_cast<size_t>(x) * samples * 4u, 12);
      }
    }
    row += rows;
  }
  if (row < height) {
    if (error) *error = "HDR TIFF float strips do not cover the image: " + path;
    return FloatRead::kError;
  }
  if (info->has_icc && info->icc.primaries != IccPrimaries::kRec2020) {
    const bool p3 = info->icc.primaries == IccPrimaries::kDisplayP3;
    for (size_t i = 0; i < out->rgb.size(); i += 3) {
      const LinearRgb src{out->rgb[i], out->rgb[i + 1], out->rgb[i + 2]};
      const LinearRgb rec = p3 ? display_p3_to_rec2020(src) : linear_srgb_to_rec2020(src);
      out->rgb[i] = rec.r;
      out->rgb[i + 1] = rec.g;
      out->rgb[i + 2] = rec.b;
    }
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

bool describe_float_tiff(const std::string& path, FloatTiffInfo* info, std::string* error) {
  RawFloatTiff raw;
  return read_float_tiff(path, false, &raw, info, error) != FloatRead::kError;
}

void log_float_tiff_fallback(const std::string& path) {
  FloatTiffInfo info;
  std::string err;
  describe_float_tiff(path, &info, &err);
  std::fprintf(stderr, "HDR TIFF: portable reader passed (%s); using the OS decoder\n",
               info.why.empty() ? err.c_str() : info.why.c_str());
}

int probe_float_tiff_portable(const std::string& path, unsigned* master_w, unsigned* master_h,
                              std::string* error) {
  if (!master_w || !master_h) {
    if (error) *error = "internal: null size output";
    return -1;
  }
  RawFloatTiff raw;
  FloatTiffInfo info;
  const FloatRead kind = read_float_tiff(path, false, &raw, &info, error);
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

int load_float_tiff_portable(const std::string& path, RawImageHolder* out, std::string* error,
                             unsigned master_w, unsigned master_h, const CropRect* crop,
                             unsigned dst_w, unsigned dst_h) {
  if (!out) {
    if (error) *error = "internal: null output";
    return -1;
  }
  RawFloatTiff raw;
  FloatTiffInfo info;
  const FloatRead kind = read_float_tiff(path, true, &raw, &info, error);
  if (kind == FloatRead::kError) return -1;
  if (kind == FloatRead::kFallback) return 0;
  out->reset();
  if (!publish_raw(raw, out, error, master_w, master_h, crop, dst_w, dst_h)) return -1;
  return 1;
}

}  // namespace uhdr_repack
