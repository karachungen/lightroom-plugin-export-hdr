#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "gui/sdr_preview_image.h"

#include <QByteArray>
#include <QImage>

#include <algorithm>
#include <iostream>
#include <vector>

namespace uhdr_repack {
namespace {

bool source_size(CGImageSourceRef source, int* width, int* height, std::string* error) {
  CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr);
  if (props == nullptr) {
    if (error) *error = "ImageIO did not return image properties";
    return false;
  }
  const CFNumberRef wnum =
      static_cast<CFNumberRef>(CFDictionaryGetValue(props, kCGImagePropertyPixelWidth));
  const CFNumberRef hnum =
      static_cast<CFNumberRef>(CFDictionaryGetValue(props, kCGImagePropertyPixelHeight));
  int w = 0;
  int h = 0;
  const bool ok = wnum && hnum && CFNumberGetValue(wnum, kCFNumberIntType, &w) &&
                  CFNumberGetValue(hnum, kCFNumberIntType, &h) && w > 0 && h > 0;
  CFRelease(props);
  if (!ok) {
    if (error) *error = "ImageIO image has no pixel size";
    return false;
  }
  *width = w;
  *height = h;
  return true;
}

CGImageSourceRef open_source(const std::string& path, std::string* error) {
  CFStringRef cfpath = CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.data()),
      static_cast<CFIndex>(path.size()), kCFStringEncodingUTF8, false);
  if (cfpath == nullptr) {
    if (error) *error = "Could not encode SDR path";
    return nullptr;
  }
  CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, cfpath, kCFURLPOSIXPathStyle, false);
  CFRelease(cfpath);
  if (url == nullptr) {
    if (error) *error = "Could not open SDR path: " + path;
    return nullptr;
  }
  CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
  CFRelease(url);
  if (source == nullptr && error) *error = "ImageIO could not open: " + path;
  return source;
}

bool cgimage_to_qimage(CGImageRef image, const std::string& path, QImage* out, std::string* error) {
  const int w = static_cast<int>(CGImageGetWidth(image));
  const int h = static_cast<int>(CGImageGetHeight(image));
  if (w <= 0 || h <= 0) {
    if (error) *error = "ImageIO decoded an empty image: " + path;
    return false;
  }

  QImage rgba(w, h, QImage::Format_RGBA8888);
  rgba.fill(0);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      rgba.bits(), static_cast<size_t>(w), static_cast<size_t>(h), 8, rgba.bytesPerLine(), space,
      kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(space);
  if (ctx == nullptr) {
    if (error) *error = "Could not create a bitmap for: " + path;
    return false;
  }
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
  CGContextRelease(ctx);
  *out = std::move(rgba);
  return true;
}

}  // namespace

bool sdr_preview_size(const std::string& path, int* width, int* height, std::string* error) {
  if (!width || !height) {
    if (error) *error = "invalid arguments";
    return false;
  }
  CGImageSourceRef source = open_source(path, error);
  if (source == nullptr) return false;
  const bool ok = source_size(source, width, height, error);
  CFRelease(source);
  return ok;
}

bool load_sdr_preview_image(const std::string& path, QImage* out, std::string* error, int max_edge) {
  if (!out) {
    if (error) *error = "invalid arguments";
    return false;
  }
  CGImageSourceRef source = open_source(path, error);
  if (source == nullptr) return false;

  int src_w = 0;
  int src_h = 0;
  if (!source_size(source, &src_w, &src_h, error)) {
    CFRelease(source);
    return false;
  }
  int dst_w = src_w;
  int dst_h = src_h;
  const bool shrink = max_edge >= 2 &&
                      fit_preview_long_edge(src_w, src_h, max_edge, &dst_w, &dst_h) &&
                      (dst_w < src_w || dst_h < src_h);

  CGImageRef image = nullptr;
  if (shrink) {
    const int max_px = std::max(dst_w, dst_h);
    CFNumberRef max_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &max_px);
    const void* keys[] = {kCGImageSourceThumbnailMaxPixelSize,
                          kCGImageSourceCreateThumbnailFromImageAlways,
                          kCGImageSourceCreateThumbnailWithTransform};
    const void* vals[] = {max_num, kCFBooleanTrue, kCFBooleanTrue};
    CFDictionaryRef opts = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 3,
                                              &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    image = CGImageSourceCreateThumbnailAtIndex(source, 0, opts);
    CFRelease(opts);
    CFRelease(max_num);
  } else {
    image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  }
  CFRelease(source);
  if (image == nullptr) {
    if (error) *error = "ImageIO could not decode: " + path;
    return false;
  }
  const bool ok = cgimage_to_qimage(image, path, out, error);
  CGImageRelease(image);
  return ok;
}

int probe_sdr_main(const std::string& path) {
  QImage image;
  std::string error;
  if (!load_sdr_preview_image(path, &image, &error) || image.isNull()) {
    std::cerr << (error.empty() ? "Could not load SDR image" : error) << "\n";
    return 1;
  }
  std::cout << "sdr: " << image.width() << "x" << image.height() << "\n";
  return 0;
}

bool encode_preview_jpeg(const QImage& image, int quality, std::vector<uint8_t>* out,
                         std::string* error) {
  if (!out || image.isNull() || image.width() <= 0 || image.height() <= 0) {
    if (error) *error = "invalid arguments";
    return false;
  }
  const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
  CGDataProviderRef provider = CGDataProviderCreateWithData(
      nullptr, rgba.constBits(), static_cast<size_t>(rgba.sizeInBytes()), nullptr);
  if (provider == nullptr) {
    if (error) *error = "Could not wrap preview pixels";
    return false;
  }
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef cg = CGImageCreate(
      static_cast<size_t>(rgba.width()), static_cast<size_t>(rgba.height()), 8, 32,
      static_cast<size_t>(rgba.bytesPerLine()), space,
      kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big, provider, nullptr, false,
      kCGRenderingIntentDefault);
  CGColorSpaceRelease(space);
  CGDataProviderRelease(provider);
  if (cg == nullptr) {
    if (error) *error = "Could not create a preview bitmap";
    return false;
  }

  CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
  CGImageDestinationRef dest =
      CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, nullptr);
  const float q = static_cast<float>(std::clamp(quality, 1, 100)) / 100.f;
  CFNumberRef qnum = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &q);
  const void* keys[] = {kCGImageDestinationLossyCompressionQuality};
  const void* vals[] = {qnum};
  CFDictionaryRef props = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
  bool ok = false;
  if (dest != nullptr) {
    CGImageDestinationAddImage(dest, cg, props);
    ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
  }
  CFRelease(props);
  CFRelease(qnum);
  CGImageRelease(cg);
  if (!ok || data == nullptr || CFDataGetLength(data) <= 0) {
    if (data) CFRelease(data);
    if (error) *error = "ImageIO JPEG encode failed";
    return false;
  }
  const auto* bytes = CFDataGetBytePtr(data);
  const CFIndex n = CFDataGetLength(data);
  out->assign(bytes, bytes + n);
  CFRelease(data);
  return true;
}

int encode_preview_jpeg_main(const std::string& path) {
  QImage image;
  std::string error;
  if (!load_sdr_preview_image(path, &image, &error) || image.isNull()) {
    std::cerr << (error.empty() ? "Could not load SDR image" : error) << "\n";
    return 1;
  }
  std::vector<uint8_t> jpeg;
  if (!encode_preview_jpeg(image, 80, &jpeg, &error) || jpeg.empty()) {
    std::cerr << (error.empty() ? "Could not encode preview JPEG" : error) << "\n";
    return 1;
  }
  const QByteArray bytes(reinterpret_cast<const char*>(jpeg.data()), static_cast<int>(jpeg.size()));
  std::cout << "data:image/jpeg;base64," << bytes.toBase64().constData() << "\n";
  return 0;
}

}  // namespace uhdr_repack
