#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreGraphics/CoreGraphics.h>

#include "gainmap_compute.h"
#include "gainmap_loader.h"
#include "half_float.h"
#include "path_io.h"
#include "color_primaries.h"
#include "tiff_input.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace uhdr_repack {

namespace {

float srgb_byte_to_linear(float c) {
  c /= 255.f;
  if (c <= 0.04045f) {
    return c / 12.92f;
  }
  return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float luminance_bt709(float r, float g, float b) {
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

bool load_sdr_linear_rgba(const std::string& path, unsigned master_width, unsigned master_height,
                          unsigned out_w, unsigned out_h, unsigned crop_x, unsigned crop_y,
                          std::vector<float>* rgba, std::string* error) {
  rgba->assign(static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 4, 0.f);

  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CIImage* im = [CIImage imageWithContentsOfURL:url];
    if (im == nil) {
      if (error) *error = "Could not load SDR: " + path;
      return false;
    }

    CGRect srcExtent = [im extent];
    const double sw = std::max(1.0, srcExtent.size.width);
    const double sh = std::max(1.0, srcExtent.size.height);
    const CGFloat sx = (CGFloat)master_width / (CGFloat)sw;
    const CGFloat sy = (CGFloat)master_height / (CGFloat)sh;

    CIImage* norm = [im imageByApplyingTransform:CGAffineTransformMakeTranslation(
                                                      -CGRectGetMinX(srcExtent), -CGRectGetMinY(srcExtent))];
    CIImage* scaled = [norm imageByApplyingTransform:CGAffineTransformMakeScale(sx, sy)];
    const CGRect outRect = CGRectMake((CGFloat)crop_x, (CGFloat)crop_y, (CGFloat)out_w, (CGFloat)out_h);
    CIImage* cropped = [scaled imageByCroppingToRect:outRect];

    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!srgb) {
      if (error) *error = "Could not create sRGB color space";
      return false;
    }
    CIContext* ctx = [CIContext contextWithOptions:@{kCIContextWorkingColorSpace : (__bridge id)srgb}];
    if (!ctx) {
      CGColorSpaceRelease(srgb);
      if (error) *error = "Could not create CIContext";
      return false;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 4);
    [ctx render:cropped
        toBitmap:buf.data()
        rowBytes:(size_t)out_w * 4
          bounds:outRect
          format:kCIFormatRGBA8
      colorSpace:srgb];
    CGColorSpaceRelease(srgb);

    for (unsigned y = 0; y < out_h; ++y) {
      for (unsigned x = 0; x < out_w; ++x) {
        const size_t i = (static_cast<size_t>(y) * out_w + x) * 4;
        (*rgba)[i + 0] = srgb_byte_to_linear(static_cast<float>(buf[i + 0]));
        (*rgba)[i + 1] = srgb_byte_to_linear(static_cast<float>(buf[i + 1]));
        (*rgba)[i + 2] = srgb_byte_to_linear(static_cast<float>(buf[i + 2]));
        (*rgba)[i + 3] = 1.f;
      }
    }
  }
  return true;
}

bool gain_from_buffers(const std::vector<float>& sdr_linear, const std::vector<float>& hdr_linear,
                       unsigned width, unsigned height, std::vector<float>* gain) {
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  gain->resize(pixels);
  for (size_t p = 0; p < pixels; ++p) {
    const size_t i = p * 4;
    const float sdr_l = luminance_bt709(sdr_linear[i], sdr_linear[i + 1], sdr_linear[i + 2]);
    const float hdr_l = luminance_bt709(hdr_linear[i], hdr_linear[i + 1], hdr_linear[i + 2]);
    float g = hdr_l / std::max(sdr_l, 1e-4f);
    g = std::clamp(g, 1.0f, 1000.0f);
    (*gain)[p] = g;
  }
  return true;
}

bool hdr_half_to_linear(const uhdr_raw_image_t& hdr, std::vector<float>* rgba) {
  if (hdr.fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || !hdr.planes[UHDR_PLANE_PACKED]) {
    return false;
  }
  const size_t pixels = static_cast<size_t>(hdr.w) * static_cast<size_t>(hdr.h);
  rgba->resize(pixels * 4);
  const auto* half = static_cast<const uint16_t*>(hdr.planes[UHDR_PLANE_PACKED]);
  for (size_t p = 0; p < pixels; ++p) {
    const size_t i = p * 4;
    const LinearRgb rec709 = rec2020_to_linear_srgb(
        {half_to_float(half[i]), half_to_float(half[i + 1]), half_to_float(half[i + 2])});
    (*rgba)[i] = rec709.r;
    (*rgba)[i + 1] = rec709.g;
    (*rgba)[i + 2] = rec709.b;
    (*rgba)[i + 3] = half_to_float(half[i + 3]);
  }
  return true;
}

}  // namespace

bool compute_auto_gainmap(const std::string& sdr_path, const std::string& hdr_tiff_path,
                          std::vector<float>* gain, int* width, int* height, std::string* error) {
  if (!gain || !width || !height) {
    if (error) *error = "null output";
    return false;
  }

  unsigned master_w = 0;
  unsigned master_h = 0;
  if (!probe_hdr_tiff_even_size(hdr_tiff_path, &master_w, &master_h, error)) {
    return false;
  }

  RawImageHolder hdr;
  if (!load_hdr_tiff_raw(hdr_tiff_path, &hdr, error)) {
    return false;
  }

  std::vector<float> sdr_linear;
  if (!load_sdr_linear_rgba(sdr_path, master_w, master_h, master_w, master_h, 0, 0, &sdr_linear,
                            error)) {
    return false;
  }

  std::vector<float> hdr_linear;
  if (!hdr_half_to_linear(hdr.ref(), &hdr_linear)) {
    if (error) *error = "HDR buffer format unsupported";
    return false;
  }

  if (!gain_from_buffers(sdr_linear, hdr_linear, master_w, master_h, gain)) {
    if (error) *error = "gain map computation failed";
    return false;
  }

  *width = static_cast<int>(master_w);
  *height = static_cast<int>(master_h);
  return true;
}

bool write_gainmap_raw_file(const std::string& path, const std::vector<float>& gain, int width,
                            int height, std::string* error) {
  if (width <= 0 || height <= 0 ||
      gain.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
    if (error) *error = "gain map size mismatch";
    return false;
  }
  std::ofstream ofs = open_output_binary(path);
  if (!ofs) {
    if (error) *error = "Could not write gain map: " + path;
    return false;
  }
  const int header[2] = {width, height};
  ofs.write(reinterpret_cast<const char*>(header), sizeof(header));
  ofs.write(reinterpret_cast<const char*>(gain.data()),
            static_cast<std::streamsize>(gain.size() * sizeof(float)));
  return true;
}

bool apply_gainmap_to_hdr(RawImageHolder* hdr, const std::string& sdr_path,
                          const std::string& gainmap_path, std::string* error,
                          unsigned master_width, unsigned master_height, const CropRect* crop) {
  if (!hdr) {
    if (error) *error = "null HDR holder";
    return false;
  }

  uhdr_raw_image_t& h = hdr->ref();
  if (h.fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || !h.planes[UHDR_PLANE_PACKED]) {
    if (error) *error = "HDR must be half-float RGBA";
    return false;
  }

  std::vector<float> gain;
  int gw = 0;
  int gh = 0;
  if (!load_gainmap_raw_file(gainmap_path, &gain, &gw, &gh, error)) {
    return false;
  }

  const unsigned master_w = master_width != 0 ? master_width : h.w;
  const unsigned master_h = master_height != 0 ? master_height : h.h;
  const unsigned ox = crop ? crop->x : 0;
  const unsigned oy = crop ? crop->y : 0;
  if (gw != static_cast<int>(master_w) || gh != static_cast<int>(master_h)) {
    if (error) *error = "Gain map dimensions do not match HDR image";
    return false;
  }
  if (ox + h.w > master_w || oy + h.h > master_h) {
    if (error) *error = "Gain map crop exceeds master bounds";
    return false;
  }

  std::vector<float> sdr_linear;
  if (!load_sdr_linear_rgba(sdr_path, master_w, master_h, h.w, h.h, ox, oy, &sdr_linear, error)) {
    return false;
  }

  auto* half = static_cast<uint16_t*>(h.planes[UHDR_PLANE_PACKED]);
  for (unsigned y = 0; y < h.h; ++y) {
    for (unsigned x = 0; x < h.w; ++x) {
      const size_t pi = static_cast<size_t>(y) * h.w + x;
      const size_t gi = static_cast<size_t>(oy + y) * static_cast<size_t>(gw) + (ox + x);
      const size_t i = pi * 4;
      const float g = gain[gi];
      const LinearRgb rec2020 = linear_srgb_to_rec2020(
          {sdr_linear[i] * g, sdr_linear[i + 1] * g, sdr_linear[i + 2] * g});
      half[i] = float_to_half(rec2020.r);
      half[i + 1] = float_to_half(rec2020.g);
      half[i + 2] = float_to_half(rec2020.b);
      half[i + 3] = float_to_half(sdr_linear[i + 3]);
    }
  }
  return true;
}

}  // namespace uhdr_repack
