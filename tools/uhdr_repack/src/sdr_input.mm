#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreGraphics/CoreGraphics.h>

#include "sdr_input.h"

#include "sdr_jpeg.h"
#include "yuv_convert.h"

#include <cmath>
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
  }

  {
    std::vector<uint8_t> rgba;
    const unsigned crop_x = crop ? crop->x : 0;
    const unsigned crop_y = crop ? crop->y : 0;
    const int portable = load_sdr_jpeg_p3(path, master_width, master_height, out_w, out_h, crop_x, crop_y,
                                          &rgba, error);
    if (portable < 0) return false;
    if (portable > 0) return rgba_p3_to_sdr_raw(rgba, out_w, out_h, out, error);
    log_sdr_jpeg_fallback(path);
  }

  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CIImage* im = [CIImage imageWithContentsOfURL:url];
    if (im == nil) {
      if (error) {
        *error = "Could not load SDR base: " + path;
      }
      return false;
    }

    CGRect srcExtent = [im extent];
    const double sw = std::max(1.0, srcExtent.size.width);
    const double sh = std::max(1.0, srcExtent.size.height);
    const CGFloat mw = (CGFloat)master_width;
    const CGFloat mh = (CGFloat)master_height;
    const CGFloat sx = mw / (CGFloat)sw;
    const CGFloat sy = mh / (CGFloat)sh;

    CIImage* norm = [im imageByApplyingTransform:CGAffineTransformMakeTranslation(
                                                      -CGRectGetMinX(srcExtent), -CGRectGetMinY(srcExtent))];
    CIImage* scaled = [norm imageByApplyingTransform:CGAffineTransformMakeScale(sx, sy)];

    CGRect outRect;
    if (crop) {
      outRect = CGRectMake((CGFloat)crop->x, (CGFloat)crop->y, (CGFloat)out_w, (CGFloat)out_h);
    } else {
      outRect = CGRectMake(0, 0, mw, mh);
    }
    CIImage* cropped = [scaled imageByCroppingToRect:outRect];

    CGColorSpaceRef p3 = CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
    if (p3 == NULL) {
      if (error) {
        *error = "Could not create Display P3 color space";
      }
      return false;
    }

    NSDictionary* ctxOpts = @{kCIContextWorkingColorSpace : (__bridge id)p3};
    CIContext* ctx = [CIContext contextWithOptions:ctxOpts];
    if (ctx == nil) {
      CGColorSpaceRelease(p3);
      if (error) {
        *error = "Could not create CIContext for SDR";
      }
      return false;
    }

    const size_t bpp = 4;
    const size_t nbytes = (size_t)out_w * (size_t)out_h * bpp;
    void* buf = std::malloc(nbytes);
    if (!buf) {
      CGColorSpaceRelease(p3);
      if (error) {
        *error = "Out of memory for SDR buffer";
      }
      return false;
    }
    std::memset(buf, 0, nbytes);

    [ctx render:cropped
        toBitmap:buf
        rowBytes:(size_t)out_w * bpp
          bounds:outRect
          format:kCIFormatRGBA8
      colorSpace:p3];
    CGColorSpaceRelease(p3);

    std::vector<uint8_t> rgba(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + nbytes);
    std::free(buf);
    return rgba_p3_to_sdr_raw(rgba, out_w, out_h, out, error);
  }
}

}  // namespace uhdr_repack
