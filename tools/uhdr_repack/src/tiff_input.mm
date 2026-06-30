#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreGraphics/CoreGraphics.h>

#include "tiff_input.h"

#include <cstdio>
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

void even_normalize(unsigned orig_w, unsigned orig_h, unsigned* w, unsigned* h) {
  *w = orig_w;
  *h = orig_h;
  if (*w % 2) {
    --(*w);
  }
  if (*h % 2) {
    --(*h);
  }
}

bool open_hdr_ciimage(const std::string& path, CIImage** out_im, CGRect* out_extent,
                      std::string* error) {
  NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  NSDictionary* opts = @{@"expandToHDR" : @YES};
  CIImage* im = [CIImage imageWithContentsOfURL:url options:opts];
  if (im == nil) {
    if (error) {
      *error = "Core Image could not read HDR file: " + path;
    }
    return false;
  }
  *out_im = im;
  *out_extent = [im extent];
  return true;
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

  @autoreleasepool {
    CIImage* im = nil;
    CGRect extent = CGRectZero;
    if (!open_hdr_ciimage(path, &im, &extent, error)) {
      return false;
    }

    const unsigned orig_w = (unsigned)std::max(1.0, std::floor(extent.size.width));
    const unsigned orig_h = (unsigned)std::max(1.0, std::floor(extent.size.height));
    unsigned w = 0;
    unsigned h = 0;
    even_normalize(orig_w, orig_h, &w, &h);
    if (w < 2 || h < 2) {
      if (error) {
        *error = "Invalid HDR image extent";
      }
      return false;
    }
    *master_w = w;
    *master_h = h;
  }

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

  @autoreleasepool {
    CIImage* im = nil;
    CGRect extent = CGRectZero;
    if (!open_hdr_ciimage(path, &im, &extent, error)) {
      return false;
    }

    const unsigned orig_w = (unsigned)std::max(1.0, std::floor(extent.size.width));
    const unsigned orig_h = (unsigned)std::max(1.0, std::floor(extent.size.height));
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
      std::fprintf(stderr,
                   "HDR dimensions cropped from %ux%u to %ux%u for 4:2:0 compatibility\n",
                   orig_w, orig_h, norm_w, norm_h);
    }

    unsigned w = crop ? crop->w : norm_w;
    unsigned h = crop ? crop->h : norm_h;
    const CGFloat ox = CGRectGetMinX(extent) + (crop ? (CGFloat)crop->x : 0.0);
    const CGFloat oy = CGRectGetMinY(extent) + (crop ? (CGFloat)crop->y : 0.0);
    const CGRect renderBounds = CGRectMake(ox, oy, (CGFloat)w, (CGFloat)h);

    CGColorSpaceRef linear2020 = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearITUR_2020);
    if (linear2020 == NULL) {
      if (error) {
        *error = "Could not create linear BT.2020 color space";
      }
      return false;
    }

    NSDictionary* ctxOpts = @{
      kCIContextUseSoftwareRenderer : @NO,
      kCIContextWorkingColorSpace : (__bridge id)linear2020,
    };
    CIContext* ctx = [CIContext contextWithOptions:ctxOpts];
    if (ctx == nil) {
      CGColorSpaceRelease(linear2020);
      if (error) {
        *error = "Could not create CIContext";
      }
      return false;
    }

    const size_t fbytes = (size_t)w * (size_t)h * 16;
    std::vector<float> fbuf(fbytes / sizeof(float));

    [ctx render:im
        toBitmap:fbuf.data()
        rowBytes:(size_t)w * 16
          bounds:renderBounds
          format:kCIFormatRGBAf
      colorSpace:linear2020];
    CGColorSpaceRelease(linear2020);

    const size_t hcount = (size_t)w * (size_t)h * 4;
    void* hdata = std::malloc(hcount * sizeof(fp16_t));
    if (!hdata) {
      if (error) {
        *error = "Out of memory allocating HDR buffer";
      }
      return false;
    }
    float_rgba_to_half_rgba(fbuf.data(), reinterpret_cast<fp16_t*>(hdata), hcount);
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
