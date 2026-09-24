#ifdef __APPLE__

#include "icc_profile.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace uhdr_repack {
namespace {

bool profile_contains(CFDataRef icc, const char* description) {
  if (!icc || !description) return false;
  const char* bytes = reinterpret_cast<const char*>(CFDataGetBytePtr(icc));
  const size_t n = static_cast<size_t>(CFDataGetLength(icc));
  const std::string hay(bytes, bytes + n);
  return hay.find(description) != std::string::npos;
}

}  // namespace

bool icc_accepted_by_os(const uint8_t* data, std::size_t size, std::string* error) {
  if (!data || size < 128) {
    if (error) *error = "ICC profile is empty";
    return false;
  }
  CFDataRef cf = CFDataCreate(kCFAllocatorDefault, data, static_cast<CFIndex>(size));
  if (!cf) {
    if (error) *error = "could not wrap the ICC profile";
    return false;
  }
  CGColorSpaceRef cs = CGColorSpaceCreateWithICCData(cf);
  CFRelease(cf);
  if (!cs) {
    if (error) *error = "CoreGraphics rejected the ICC profile";
    return false;
  }
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  const CGFloat comps[3] = {0.7, 0.15, 0.05};
  CGColorRef src = CGColorCreate(cs, comps);
  CGColorRef dst = nullptr;
  if (srgb && src) {
    dst = CGColorCreateCopyByMatchingToColorSpace(srgb, kCGRenderingIntentDefault, src, nullptr);
  }
  const bool ok = dst != nullptr;
  if (dst) CGColorRelease(dst);
  if (src) CGColorRelease(src);
  if (srgb) CGColorSpaceRelease(srgb);
  CGColorSpaceRelease(cs);
  if (!ok && error) *error = "CoreGraphics could not color-transform with the ICC profile";
  return ok;
}

bool image_has_color_profile(const std::string& path, const char* description_substring,
                             std::string* error) {
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    NSDictionary* opts = @{@"expandToHDR" : @YES};
    CIImage* image = [CIImage imageWithContentsOfURL:url options:opts];
    if (image == nil) {
      if (error) *error = "Core Image could not open " + path;
      return false;
    }
    CGColorSpaceRef cs = image.colorSpace;
    if (!cs) {
      if (error) *error = "image has no embedded color profile";
      return false;
    }
    CFDataRef icc = CGColorSpaceCopyICCData(cs);
    const bool ok = profile_contains(icc, description_substring);
    if (icc) CFRelease(icc);
    if (!ok && error) {
      *error = std::string("embedded profile is missing ") +
               (description_substring ? description_substring : "");
    }
    return ok;
  }
}

bool read_tiff_float_rgb_os(const std::string& path, std::vector<float>* rgb, int* width, int* height,
                            std::string* error) {
  if (!rgb || !width || !height) {
    if (error) *error = "internal: null TIFF output";
    return false;
  }
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    NSDictionary* opts = @{@"expandToHDR" : @YES};
    CIImage* image = [CIImage imageWithContentsOfURL:url options:opts];
    if (image == nil) {
      if (error) *error = "Core Image could not open " + path;
      return false;
    }
    CGRect extent = image.extent;
    if (!std::isfinite(extent.size.width) || !std::isfinite(extent.size.height) || extent.size.width < 2 ||
        extent.size.height < 2) {
      if (error) *error = "Core Image reported an invalid TIFF size";
      return false;
    }
    const int w = static_cast<int>(extent.size.width);
    const int h = static_cast<int>(extent.size.height);
    CGColorSpaceRef linear2020 = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearITUR_2020);
    if (!linear2020) {
      if (error) *error = "could not create extended linear Rec.2020";
      return false;
    }
    CIContext* ctx = [CIContext contextWithOptions:@{
      kCIContextWorkingColorSpace : (__bridge id)linear2020,
    }];
    std::vector<float> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0.0f);
    [ctx render:image
        toBitmap:rgba.data()
        rowBytes:static_cast<size_t>(w) * 16u
          bounds:extent
          format:kCIFormatRGBAf
      colorSpace:linear2020];
    CGColorSpaceRelease(linear2020);
    rgb->assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u, 0.0f);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const size_t s = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
        const size_t d = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3u;
        (*rgb)[d] = rgba[s];
        (*rgb)[d + 1] = rgba[s + 1];
        (*rgb)[d + 2] = rgba[s + 2];
      }
    }
    *width = w;
    *height = h;
    return true;
  }
}

}  // namespace uhdr_repack

#endif
