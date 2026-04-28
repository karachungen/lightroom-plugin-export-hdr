#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreGraphics/CoreGraphics.h>

#include "sdr_input.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace uhdr_repack {

namespace {

/** BT.709 full-range digital Y'CbCr 8-bit (matches typical JPEG/JFIF mapping). */
void rgb888_to_y_cb_cr_bt709(float r, float g, float b, float* y_out, float* cb_out,
                             float* cr_out) {
  constexpr float kr = 0.2126f;
  constexpr float kg = 0.7152f;
  constexpr float kb = 0.0722f;
  float y = kr * r + kg * g + kb * b;
  float cb = 128.f + (0.5f * (b - y)) / (1.f - kb);
  float cr = 128.f + (0.5f * (r - y)) / (1.f - kr);
  *y_out = y;
  *cb_out = cb;
  *cr_out = cr;
}

bool rgba8888_to_yuv420_bt709(const uint8_t* rgba, unsigned w, unsigned h, uint8_t** plane_y,
                              uint8_t** plane_u, uint8_t** plane_v, std::string* error) {
  if (w % 2 || h % 2) {
    if (error) *error = "SDR dimensions must be even for 4:2:0 YCbCr";
    return false;
  }
  const unsigned cw = w / 2;
  const unsigned ch = h / 2;
  const size_t n_y = (size_t)w * (size_t)h;
  const size_t n_c = (size_t)cw * (size_t)ch;

  auto y_buf = std::unique_ptr<uint8_t[]>(new uint8_t[n_y]);
  auto u_buf = std::unique_ptr<uint8_t[]>(new uint8_t[n_c]);
  auto v_buf = std::unique_ptr<uint8_t[]>(new uint8_t[n_c]);

  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      size_t i = ((size_t)y * w + x) * 4;
      float rf = rgba[i];
      float gf = rgba[i + 1];
      float bf = rgba[i + 2];
      float yv, cb, cr;
      rgb888_to_y_cb_cr_bt709(rf, gf, bf, &yv, &cb, &cr);
      uint8_t y8 = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(yv))));
      y_buf.get()[(size_t)y * w + x] = y8;
    }
  }

  for (unsigned by = 0; by < ch; ++by) {
    for (unsigned bx = 0; bx < cw; ++bx) {
      float acc_r = 0.f, acc_g = 0.f, acc_b = 0.f;
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          unsigned x = bx * 2 + static_cast<unsigned>(dx);
          unsigned yy = by * 2 + static_cast<unsigned>(dy);
          size_t i = ((size_t)yy * w + x) * 4;
          acc_r += rgba[i];
          acc_g += rgba[i + 1];
          acc_b += rgba[i + 2];
        }
      }
      acc_r *= 0.25f;
      acc_g *= 0.25f;
      acc_b *= 0.25f;
      float yv, cb, cr;
      rgb888_to_y_cb_cr_bt709(acc_r, acc_g, acc_b, &yv, &cb, &cr);
      uint8_t cb8 = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(cb))));
      uint8_t cr8 = static_cast<uint8_t>(std::min(255.f, std::max(0.f, std::round(cr))));
      size_t ci = (size_t)by * cw + bx;
      u_buf.get()[ci] = cb8;
      v_buf.get()[ci] = cr8;
    }
  }

  *plane_y = y_buf.release();
  *plane_u = u_buf.release();
  *plane_v = v_buf.release();
  return true;
}

}  // namespace

bool load_sdr_base_raw(const std::string& path, unsigned target_width, unsigned target_height,
                       RawImageHolder* out, std::string* error) {
  if (!out || target_width == 0 || target_height == 0) {
    if (error) *error = "invalid arguments";
    return false;
  }
  out->reset();

  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CIImage* im = [CIImage imageWithContentsOfURL:url];
    if (im == nil) {
      if (error) *error = "Could not load SDR base: " + path;
      return false;
    }

    CGRect srcExtent = [im extent];
    const double sw = std::max(1.0, srcExtent.size.width);
    const double sh = std::max(1.0, srcExtent.size.height);
    const CGFloat tw = (CGFloat)target_width;
    const CGFloat th = (CGFloat)target_height;
    const CGFloat sx = tw / (CGFloat)sw;
    const CGFloat sy = th / (CGFloat)sh;

    CIImage* norm = [im imageByApplyingTransform:CGAffineTransformMakeTranslation(
                                                      -CGRectGetMinX(srcExtent), -CGRectGetMinY(srcExtent))];
    CIImage* scaled =
        [norm imageByApplyingTransform:CGAffineTransformMakeScale(sx, sy)];
    CGRect outRect = CGRectMake(0, 0, tw, th);
    CIImage* cropped = [scaled imageByCroppingToRect:outRect];

    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (srgb == NULL) {
      if (error) *error = "Could not create sRGB color space";
      return false;
    }

    NSDictionary* ctxOpts = @{kCIContextWorkingColorSpace : (__bridge id)srgb};
    CIContext* ctx = [CIContext contextWithOptions:ctxOpts];
    if (ctx == nil) {
      CGColorSpaceRelease(srgb);
      if (error) *error = "Could not create CIContext for SDR";
      return false;
    }

    const size_t bpp = 4;
    const size_t nbytes = (size_t)target_width * (size_t)target_height * bpp;
    void* buf = std::malloc(nbytes);
    if (!buf) {
      CGColorSpaceRelease(srgb);
      if (error) *error = "Out of memory for SDR buffer";
      return false;
    }
    std::memset(buf, 0, nbytes);

    [ctx render:cropped
        toBitmap:buf
        rowBytes:(size_t)target_width * bpp
          bounds:outRect
          format:kCIFormatRGBA8
      colorSpace:srgb];
    CGColorSpaceRelease(srgb);

    uint8_t* py = nullptr;
    uint8_t* pu = nullptr;
    uint8_t* pv = nullptr;
    if (!rgba8888_to_yuv420_bt709(static_cast<const uint8_t*>(buf), target_width, target_height,
                                  &py, &pu, &pv, error)) {
      std::free(buf);
      return false;
    }
    std::free(buf);

    uhdr_raw_image_t& r = out->ref();
    std::memset(&r, 0, sizeof(r));
    r.fmt = UHDR_IMG_FMT_12bppYCbCr420;
    r.cg = UHDR_CG_BT_709;
    r.ct = UHDR_CT_SRGB;
    r.range = UHDR_CR_FULL_RANGE;
    r.w = target_width;
    r.h = target_height;
    r.planes[UHDR_PLANE_Y] = py;
    r.planes[UHDR_PLANE_U] = pu;
    r.planes[UHDR_PLANE_V] = pv;
    r.stride[UHDR_PLANE_Y] = target_width;
    r.stride[UHDR_PLANE_U] = target_width / 2;
    r.stride[UHDR_PLANE_V] = target_width / 2;
  }

  return true;
}

}  // namespace uhdr_repack
