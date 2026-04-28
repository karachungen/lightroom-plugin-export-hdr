#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreGraphics/CoreGraphics.h>

#include "tiff_input.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace uhdr_repack {

namespace {

#if defined(__APPLE__)
typedef __fp16 fp16_t;
#else
typedef uint16_t fp16_t;
#endif

void float_rgba_to_half_rgba(const float* src, fp16_t* dst, size_t num_floats) {
  for (size_t i = 0; i < num_floats; ++i) {
    float f = src[i];
    if (!std::isfinite(f)) {
      f = 0.f;
    }
    dst[i] = static_cast<fp16_t>(f);
  }
}

}  // namespace

bool load_hdr_tiff_raw(const std::string& path, RawImageHolder* out, std::string* error) {
  if (!out) {
    if (error) *error = "internal: null output";
    return false;
  }
  out->reset();

  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    NSDictionary* opts = @{@"expandToHDR" : @YES};

    CIImage* im = [CIImage imageWithContentsOfURL:url options:opts];
    if (im == nil) {
      if (error) *error = "Core Image could not read HDR file: " + path;
      return false;
    }

    CGRect extent = [im extent];
    unsigned w = (unsigned)std::max(1.0, std::floor(extent.size.width));
    unsigned h = (unsigned)std::max(1.0, std::floor(extent.size.height));
    if (w == 0 || h == 0) {
      if (error) *error = "Invalid image extent";
      return false;
    }

    CGColorSpaceRef linear2020 = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearITUR_2020);
    if (linear2020 == NULL) {
      if (error) *error = "Could not create linear BT.2020 color space";
      return false;
    }

    NSDictionary* ctxOpts = @{
      kCIContextUseSoftwareRenderer : @NO,
      kCIContextWorkingColorSpace : (__bridge id)linear2020,
    };
    CIContext* ctx = [CIContext contextWithOptions:ctxOpts];
    if (ctx == nil) {
      CGColorSpaceRelease(linear2020);
      if (error) *error = "Could not create CIContext";
      return false;
    }

    const size_t fbytes = (size_t)w * (size_t)h * 16;  // RGBA float
    std::vector<float> fbuf(fbytes / sizeof(float));

    [ctx render:im
        toBitmap:fbuf.data()
        rowBytes:(size_t)w * 16
          bounds:extent
          format:kCIFormatRGBAf
      colorSpace:linear2020];
    CGColorSpaceRelease(linear2020);

    const size_t hcount = (size_t)w * (size_t)h * 4;
    void* hdata = std::malloc(hcount * sizeof(fp16_t));
    if (!hdata) {
      if (error) *error = "Out of memory allocating HDR buffer";
      return false;
    }
    float_rgba_to_half_rgba(fbuf.data(), reinterpret_cast<fp16_t*>(hdata), hcount);
    // Drop full-float buffer before encode; peak memory is high (RGBA float + half + uhdr).
    fbuf.clear();
    fbuf.shrink_to_fit();

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
  }

  return true;
}

}  // namespace uhdr_repack
