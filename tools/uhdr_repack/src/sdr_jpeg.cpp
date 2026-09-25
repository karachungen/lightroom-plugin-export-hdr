#include "sdr_jpeg.h"

#include "color_primaries.h"
#include "jpeg_container.h"
#include "yuv_convert.h"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstring>

extern "C" {
#ifndef _WIN32
#ifndef HAVE_BOOLEAN
#define HAVE_BOOLEAN
typedef int boolean;
#endif
#endif
#include <jpeglib.h>
}

namespace uhdr_repack {

namespace {

struct JpegError {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void jpeg_error_exit(j_common_ptr cinfo) { longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jump, 1); }

bool read_jpeg_header(const std::vector<uint8_t>& jpeg, unsigned* w, unsigned* h, int* color_space) {
  jpeg_decompress_struct cinfo{};
  JpegError jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, jpeg.data(), static_cast<unsigned long>(jpeg.size()));
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  *w = cinfo.image_width;
  *h = cinfo.image_height;
  *color_space = static_cast<int>(cinfo.jpeg_color_space);
  jpeg_destroy_decompress(&cinfo);
  return true;
}

}  // namespace

bool decode_jpeg_rgba8(const std::vector<uint8_t>& jpeg, std::vector<uint8_t>& rgba, unsigned* width,
                       unsigned* height, std::string* error) {
  if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
    if (error) *error = "Not a JPEG";
    return false;
  }
  jpeg_decompress_struct cinfo{};
  JpegError jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_decompress(&cinfo);
    if (error) *error = "libjpeg failed to decompress SDR JPEG";
    return false;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, jpeg.data(), static_cast<unsigned long>(jpeg.size()));
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    if (error) *error = "libjpeg could not read JPEG header";
    return false;
  }
  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);
  const unsigned w = cinfo.output_width;
  const unsigned h = cinfo.output_height;
  if (w < 1 || h < 1 || cinfo.output_components != 3) {
    jpeg_destroy_decompress(&cinfo);
    if (error) *error = "JPEG has empty extent";
    return false;
  }
  rgba.assign(static_cast<size_t>(w) * h * 4u, 255);
  std::vector<uint8_t> row(static_cast<size_t>(w) * 3u);
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW rows[1] = {row.data()};
    jpeg_read_scanlines(&cinfo, rows, 1);
    uint8_t* dst = rgba.data() + static_cast<size_t>(cinfo.output_scanline - 1) * w * 4u;
    for (unsigned x = 0; x < w; ++x) {
      dst[0] = row[x * 3u];
      dst[1] = row[x * 3u + 1];
      dst[2] = row[x * 3u + 2];
      dst[3] = 255;
      dst += 4;
    }
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  if (width) *width = w;
  if (height) *height = h;
  return true;
}

bool scale_crop_rgba8_buffer(const std::vector<uint8_t>& src, unsigned src_w, unsigned src_h,
                             unsigned master_w, unsigned master_h, unsigned out_w, unsigned out_h,
                             unsigned crop_x, unsigned crop_y, std::vector<uint8_t>& dst,
                             std::string* error) {
  if (src_w < 1 || src_h < 1 || master_w < 1 || master_h < 1 || out_w < 1 || out_h < 1) {
    if (error) *error = "Invalid SDR scale/crop size";
    return false;
  }
  if (crop_x + out_w > master_w || crop_y + out_h > master_h) {
    if (error) *error = "SDR crop exceeds master bounds";
    return false;
  }
  auto sample = [&](unsigned x, unsigned y, uint8_t* px) {
    x = std::min(x, src_w - 1);
    y = std::min(y, src_h - 1);
    const uint8_t* s = src.data() + (static_cast<size_t>(y) * src_w + x) * 4u;
    px[0] = s[0];
    px[1] = s[1];
    px[2] = s[2];
    px[3] = 255;
  };
  dst.assign(static_cast<size_t>(out_w) * out_h * 4u, 255);
  for (unsigned y = 0; y < out_h; ++y) {
    const float fy = (src_h / static_cast<float>(master_h)) * static_cast<float>(crop_y + y);
    const unsigned sy = std::min(src_h - 1, static_cast<unsigned>(fy));
    for (unsigned x = 0; x < out_w; ++x) {
      const float fx = (src_w / static_cast<float>(master_w)) * static_cast<float>(crop_x + x);
      const unsigned sx = std::min(src_w - 1, static_cast<unsigned>(fx));
      sample(sx, sy, dst.data() + (static_cast<size_t>(y) * out_w + x) * 4u);
    }
  }
  return true;
}

bool describe_sdr_jpeg(const std::string& path, SdrJpegInfo* info, std::string* error) {
  *info = SdrJpegInfo{};
  std::vector<uint8_t> file;
  if (!load_binary_file(path, &file, error)) return false;
  if (file.size() < 4 || file[0] != 0xff || file[1] != 0xd8) {
    info->why = "not a JPEG";
    return true;
  }
  info->is_jpeg = true;
  int color_space = 0;
  if (!read_jpeg_header(file, &info->width, &info->height, &color_space)) {
    info->why = "libjpeg could not read the header";
    return true;
  }
  if (color_space != JCS_YCbCr && color_space != JCS_GRAYSCALE && color_space != JCS_RGB) {
    info->why = "JPEG is CMYK or YCCK";
    return true;
  }
  std::vector<uint8_t> icc;
  std::string icc_err;
  // A missing ICC_PROFILE marker is untagged sRGB. Any other failure to read the
  // embedded profile is not sRGB: leave supported false so the OS decoder reads the file.
  if (!read_embedded_icc(path, &icc, &icc_err)) {
    if (icc_err == "JPEG has no ICC_PROFILE marker") {
      info->supported = true;
      return true;
    }
    info->why = icc_err.empty() ? "could not read the embedded ICC profile" : icc_err;
    return true;
  }
  info->has_icc = true;
  info->icc = classify_icc(icc);
  if (info->icc.transfer != IccTransfer::kSrgb) {
    info->why = "ICC curve is not the sRGB curve";
    return true;
  }
  if (info->icc.primaries != IccPrimaries::kDisplayP3 && info->icc.primaries != IccPrimaries::kSrgb) {
    info->why = "ICC primaries are not Display P3 or sRGB";
    return true;
  }
  info->supported = true;
  return true;
}

int load_sdr_jpeg_p3(const std::string& path, unsigned master_w, unsigned master_h, unsigned out_w,
                     unsigned out_h, unsigned crop_x, unsigned crop_y, std::vector<uint8_t>* rgba,
                     std::string* error) {
  SdrJpegInfo info;
  if (!describe_sdr_jpeg(path, &info, error)) return -1;
  if (!info.supported) return 0;
  std::vector<uint8_t> file;
  if (!load_binary_file(path, &file, error)) return -1;
  std::vector<uint8_t> decoded;
  unsigned w = 0;
  unsigned h = 0;
  if (!decode_jpeg_rgba8(file, decoded, &w, &h, error)) return -1;
  if (!scale_crop_rgba8_buffer(decoded, w, h, master_w, master_h, out_w, out_h, crop_x, crop_y, *rgba, error)) {
    return -1;
  }
  if (info.icc.primaries != IccPrimaries::kDisplayP3) {
    srgb_rgba8888_to_display_p3(rgba->data(), static_cast<size_t>(out_w) * out_h);
  }
  return 1;
}

bool rgba_p3_to_sdr_raw(const std::vector<uint8_t>& rgba, unsigned w, unsigned h, RawImageHolder* out,
                        std::string* error) {
  uint8_t* py = nullptr;
  uint8_t* pu = nullptr;
  uint8_t* pv = nullptr;
  if (!rgba8888_to_yuv420_bt601(rgba.data(), w, h, &py, &pu, &pv, error)) return false;
  out->reset();
  uhdr_raw_image_t& r = out->ref();
  std::memset(&r, 0, sizeof(r));
  r.fmt = UHDR_IMG_FMT_12bppYCbCr420;
  r.cg = UHDR_CG_DISPLAY_P3;
  r.ct = UHDR_CT_SRGB;
  r.range = UHDR_CR_FULL_RANGE;
  r.w = w;
  r.h = h;
  r.planes[UHDR_PLANE_Y] = py;
  r.planes[UHDR_PLANE_U] = pu;
  r.planes[UHDR_PLANE_V] = pv;
  r.stride[UHDR_PLANE_Y] = w;
  r.stride[UHDR_PLANE_U] = w / 2;
  r.stride[UHDR_PLANE_V] = w / 2;
  return true;
}

void log_sdr_jpeg_fallback(const std::string& path) {
  SdrJpegInfo info;
  std::string err;
  describe_sdr_jpeg(path, &info, &err);
  std::fprintf(stderr, "SDR base: portable loader passed (%s); using the OS decoder\n",
               info.why.empty() ? err.c_str() : info.why.c_str());
}

}  // namespace uhdr_repack
