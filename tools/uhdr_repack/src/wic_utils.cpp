#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "wic_utils.h"

#include "color_primaries.h"
#include "icc_profile.h"
#include "jpeg_container.h"
#include "sdr_jpeg.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <string>
#include <vector>

extern "C" {
#ifndef HAVE_BOOLEAN
#define HAVE_BOOLEAN
#endif
#include <jpeglib.h>
}

#include <windows.h>
#include <objbase.h>

namespace uhdr_repack {
namespace wic {

HRESULT hr_or_error(HRESULT hr, std::string* error, const std::string& msg) {
  if (FAILED(hr) && error) {
    *error = msg + " (HRESULT=" + std::to_string(static_cast<unsigned long>(hr)) + ")";
  }
  return hr;
}

struct JpegError {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void jpeg_error_exit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<JpegError*>(cinfo->err);
  longjmp(err->jump, 1);
}

bool convert_frame_to_format(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
                             REFWICPixelFormatGUID target_fmt,
                             Microsoft::WRL::ComPtr<IWICBitmapSource>& out_source,
                             std::string* error) {
  WICPixelFormatGUID src_fmt{};
  HRESULT hr = frame->GetPixelFormat(&src_fmt);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC GetPixelFormat failed");
    return false;
  }

  Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
  hr = factory->CreateFormatConverter(&converter);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC CreateFormatConverter failed");
    return false;
  }

  hr = converter->Initialize(frame, target_fmt, WICBitmapDitherTypeNone, nullptr, 0.f,
                             WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC format conversion failed");
    return false;
  }

  out_source = converter;
  return true;
}

bool copy_pixels(IWICBitmapSource* source, unsigned width, unsigned height,
                 WICPixelFormatGUID fmt, void* dst, size_t dst_bytes, std::string* error) {
  UINT w = 0;
  UINT h = 0;
  HRESULT hr = source->GetSize(&w, &h);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC GetSize failed");
    return false;
  }
  if (w != width || h != height) {
    if (error) {
      *error = "WIC decoded size mismatch";
    }
    return false;
  }

  UINT bpp = 0;
  if (IsEqualGUID(fmt, GUID_WICPixelFormat128bppRGBFloat)) {
    bpp = 16;
  } else if (IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBAHalf)) {
    bpp = 8;
  } else if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA)) {
    bpp = 4;
  } else {
    if (error) {
      *error = "Unsupported WIC pixel format for copy";
    }
    return false;
  }

  const UINT stride = width * bpp;
  const UINT buf_size = stride * height;
  if (static_cast<size_t>(buf_size) > dst_bytes) {
    if (error) {
      *error = "WIC destination buffer too small";
    }
    return false;
  }

  hr = source->CopyPixels(nullptr, stride, buf_size, static_cast<BYTE*>(dst));
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC CopyPixels failed");
    return false;
  }
  return true;
}

bool half_rgba_to_float_rgba(const uint8_t* src, std::vector<float>& rgba, unsigned width,
                               unsigned height) {
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  rgba.resize(pixels * 4);
  for (size_t i = 0; i < pixels * 4; ++i) {
    uint16_t bits = 0;
    std::memcpy(&bits, src + i * 2, 2);
    // Expand half to float (simple path via float32 reinterpret isn't valid; use bit expand).
    uint32_t sign = (bits & 0x8000u) << 16;
    uint32_t exp = (bits & 0x7C00u) >> 10;
    uint32_t mant = bits & 0x03FFu;
    float out = 0.f;
    if (exp == 0) {
      if (mant != 0) {
        exp = 127 - 14;
        while ((mant & 0x0400u) == 0) {
          mant <<= 1;
          --exp;
        }
        mant &= 0x03FFu;
        uint32_t fbits = sign | (exp << 23) | (mant << 13);
        std::memcpy(&out, &fbits, sizeof(out));
      }
    } else if (exp == 31) {
      uint32_t fbits = sign | 0x7F800000u | (mant << 13);
      std::memcpy(&out, &fbits, sizeof(out));
    } else {
      uint32_t fbits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
      std::memcpy(&out, &fbits, sizeof(out));
    }
    rgba[i] = out;
  }
  return true;
}

bool load_rgba_float(const std::string& path, std::vector<float>& rgba, unsigned* width,
                     unsigned* height, std::string* error) {
  auto factory = create_factory();
  if (!factory) {
    if (error) {
      *error = "WIC factory unavailable";
    }
    return false;
  }

  const std::wstring wpath = utf8_to_wide(path);
  Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
  HRESULT hr = factory->CreateDecoderFromFilename(
      wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC could not open image: " + path);
    return false;
  }

  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC GetFrame failed");
    return false;
  }

  UINT w = 0;
  UINT h = 0;
  hr = frame->GetSize(&w, &h);
  if (FAILED(hr) || w < 2 || h < 2) {
    if (error) {
      *error = "Invalid image extent";
    }
    return false;
  }

  Microsoft::WRL::ComPtr<IWICBitmapSource> float_source;
  if (!convert_frame_to_format(factory.Get(), frame.Get(), GUID_WICPixelFormat128bppRGBFloat,
                               float_source, error)) {
    Microsoft::WRL::ComPtr<IWICBitmapSource> half_source;
    if (!convert_frame_to_format(factory.Get(), frame.Get(), GUID_WICPixelFormat64bppRGBAHalf,
                                 half_source, error)) {
      return false;
    }
    std::vector<uint8_t> half_buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 8);
    if (!copy_pixels(half_source.Get(), static_cast<unsigned>(w), static_cast<unsigned>(h),
                     GUID_WICPixelFormat64bppRGBAHalf, half_buf.data(), half_buf.size(), error)) {
      return false;
    }
    if (!half_rgba_to_float_rgba(half_buf.data(), rgba, static_cast<unsigned>(w),
                                 static_cast<unsigned>(h))) {
      if (error) {
        *error = "Failed to expand half-float RGBA";
      }
      return false;
    }
  } else {
    rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    if (!copy_pixels(float_source.Get(), static_cast<unsigned>(w), static_cast<unsigned>(h),
                     GUID_WICPixelFormat128bppRGBFloat, rgba.data(), rgba.size() * sizeof(float),
                     error)) {
      return false;
    }
    // WIC 128bppRGBFloat is RGBx; promote to RGBA with A=1.
    for (size_t px = 0; px < static_cast<size_t>(w) * static_cast<size_t>(h); ++px) {
      const size_t i = px * 4;
      const float r = rgba[i];
      const float g = rgba[i + 1];
      const float b = rgba[i + 2];
      rgba[i] = r;
      rgba[i + 1] = g;
      rgba[i + 2] = b;
      rgba[i + 3] = 1.f;
    }
  }

  *width = static_cast<unsigned>(w);
  *height = static_cast<unsigned>(h);
  return true;
}

bool frame_is_display_p3(IWICBitmapFrameDecode* frame) {
  if (!frame) return false;
  UINT count = 0;
  frame->GetColorContexts(0, nullptr, &count);
  if (count == 0) return false;
  IWICColorContext* raw = nullptr;
  UINT actual = 0;
  if (FAILED(frame->GetColorContexts(1, &raw, &actual)) || actual == 0 || !raw) {
    if (raw) raw->Release();
    return false;
  }
  Microsoft::WRL::ComPtr<IWICColorContext> ctx;
  ctx.Attach(raw);
  UINT bytes = 0;
  if (FAILED(ctx->GetProfileBytes(0, nullptr, &bytes)) || bytes == 0) return false;
  std::vector<uint8_t> profile(bytes);
  UINT actual_bytes = 0;
  if (FAILED(ctx->GetProfileBytes(bytes, profile.data(), &actual_bytes))) return false;
  profile.resize(actual_bytes);
  return classify_icc(profile).primaries == IccPrimaries::kDisplayP3;
}

bool open_frame(const std::string& path, Microsoft::WRL::ComPtr<IWICImagingFactory>& factory,
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode>& frame, std::string* error) {
  factory = create_factory();
  if (!factory) {
    if (error) *error = "WIC factory unavailable";
    return false;
  }
  const std::wstring wpath = utf8_to_wide(path);
  Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
  HRESULT hr = factory->CreateDecoderFromFilename(
      wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC could not open SDR base: " + path);
    return false;
  }
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC GetFrame failed");
    return false;
  }
  return true;
}

void expand_bgr24_to_rgba8(const uint8_t* bgr, UINT stride, unsigned w, unsigned h,
                           std::vector<uint8_t>& rgba) {
  rgba.assign(static_cast<size_t>(w) * h * 4u, 255);
  for (unsigned y = 0; y < h; ++y) {
    const uint8_t* row = bgr + static_cast<size_t>(y) * stride;
    uint8_t* dst = rgba.data() + static_cast<size_t>(y) * w * 4u;
    for (unsigned x = 0; x < w; ++x) {
      dst[0] = row[2];
      dst[1] = row[1];
      dst[2] = row[0];
      dst[3] = 255;
      row += 3;
      dst += 4;
    }
  }
}

bool copy_bgr24(IWICBitmapSource* source, const WICRect* rect, unsigned w, unsigned h,
                std::vector<uint8_t>& rgba, std::string* error) {
  const UINT stride = ((w * 3u) + 3u) & ~3u;
  std::vector<uint8_t> bgr(static_cast<size_t>(stride) * h, 0);
  const HRESULT hr =
      source->CopyPixels(rect, stride, static_cast<UINT>(bgr.size()), bgr.data());
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC 24bppBGR CopyPixels failed");
    return false;
  }
  expand_bgr24_to_rgba8(bgr.data(), stride, w, h, rgba);
  return true;
}

bool scale_crop_copy_rgba8(IWICImagingFactory* factory, IWICBitmapSource* source,
                           unsigned master_width, unsigned master_height, unsigned out_w,
                           unsigned out_h, unsigned crop_x, unsigned crop_y,
                           std::vector<uint8_t>& rgba, std::string* error) {
  Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
  HRESULT hr = factory->CreateBitmapScaler(&scaler);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC CreateBitmapScaler failed");
    return false;
  }
  hr = scaler->Initialize(source, master_width, master_height,
                          WICBitmapInterpolationModeHighQualityCubic);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC scaler Initialize failed");
    return false;
  }
  // JPEG's native WIC layout is 24bppBGR. 32bppRGBA/BGRA CopyPixels channel order
  // depends on the source (Qt vs Lightroom ICC) and caused R/B swaps.
  Microsoft::WRL::ComPtr<IWICFormatConverter> bgr_source;
  hr = factory->CreateFormatConverter(&bgr_source);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC CreateFormatConverter (scaled BGR) failed");
    return false;
  }
  hr = bgr_source->Initialize(scaler.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                              nullptr, 0.f, WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC scaled BGR conversion failed");
    return false;
  }
  WICRect crop_rect{};
  crop_rect.X = static_cast<INT>(crop_x);
  crop_rect.Y = static_cast<INT>(crop_y);
  crop_rect.Width = static_cast<INT>(out_w);
  crop_rect.Height = static_cast<INT>(out_h);
  return copy_bgr24(bgr_source.Get(), &crop_rect, out_w, out_h, rgba, error);
}

bool managed_rgba8(const std::string& path, unsigned master_width, unsigned master_height,
                   unsigned out_w, unsigned out_h, unsigned crop_x, unsigned crop_y,
                   Rgba8Space space, std::vector<uint8_t>& rgba, std::string* error) {
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  if (!open_frame(path, factory, frame, error)) return false;

  std::vector<uint8_t> file;
  if (!load_binary_file(path, &file, error)) return false;

  std::vector<uint8_t> decoded;
  unsigned src_w = 0;
  unsigned src_h = 0;
  const bool is_jpeg = file.size() >= 2 && file[0] == 0xff && file[1] == 0xd8;
  if (is_jpeg) {
    // libjpeg-turbo always emits RGB. WIC CopyPixels channel order depends on the
    // JPEG (Qt vs Lightroom ICC) and was swapping R/B on Instagram crop encodes.
    if (!decode_jpeg_rgba8(file, decoded, &src_w, &src_h, error)) return false;
  } else {
    Microsoft::WRL::ComPtr<IWICBitmapSource> source;
    if (!convert_frame_to_format(factory.Get(), frame.Get(), GUID_WICPixelFormat24bppBGR, source,
                                 error)) {
      return false;
    }
    UINT w = 0;
    UINT h = 0;
    if (FAILED(frame->GetSize(&w, &h))) return false;
    src_w = w;
    src_h = h;
    if (!copy_bgr24(source.Get(), nullptr, src_w, src_h, decoded, error)) return false;
  }
  if (!scale_crop_rgba8_buffer(decoded, src_w, src_h, master_width, master_height, out_w, out_h,
                               crop_x, crop_y, rgba, error)) {
    return false;
  }
  const bool src_p3 = frame_is_display_p3(frame.Get());
  if (space == Rgba8Space::DisplayP3 && !src_p3) {
    srgb_rgba8888_to_display_p3(rgba.data(), static_cast<size_t>(out_w) * out_h);
  } else if (space == Rgba8Space::Srgb && src_p3) {
    display_p3_rgba8888_to_srgb(rgba.data(), static_cast<size_t>(out_w) * out_h);
  }
  return true;
}

Microsoft::WRL::ComPtr<IWICImagingFactory> create_factory() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    return nullptr;
  }
  return factory;
}

std::wstring utf8_to_wide(const std::string& path) {
  if (path.empty()) {
    return std::wstring();
  }
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
  if (needed <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), &wide[0], needed);
  return wide;
}

bool decode_to_rgba_float(const std::string& path, std::vector<float>& rgba, unsigned* width,
                          unsigned* height, std::string* error) {
  return load_rgba_float(path, rgba, width, height, error);
}

bool decode_scale_crop_to_rgba8(const std::string& path, unsigned master_width,
                                unsigned master_height, unsigned out_w, unsigned out_h,
                                unsigned crop_x, unsigned crop_y, std::vector<uint8_t>& rgba,
                                std::string* error, Rgba8Space space) {
  return managed_rgba8(path, master_width, master_height, out_w, out_h, crop_x, crop_y, space, rgba,
                       error);
}

bool decode_scale_crop_to_linear_p3(const std::string& path, unsigned master_width,
                                    unsigned master_height, unsigned out_w, unsigned out_h,
                                    unsigned crop_x, unsigned crop_y, std::vector<float>& rgba,
                                    std::string* error) {
  std::vector<uint8_t> bytes;
  if (!managed_rgba8(path, master_width, master_height, out_w, out_h, crop_x, crop_y,
                     Rgba8Space::DisplayP3, bytes, error)) {
    return false;
  }
  rgba.resize(bytes.size());
  for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
    rgba[i] = srgb_eotf(bytes[i] / 255.0f);
    rgba[i + 1] = srgb_eotf(bytes[i + 1] / 255.0f);
    rgba[i + 2] = srgb_eotf(bytes[i + 2] / 255.0f);
    rgba[i + 3] = 1.0f;
  }
  return true;
}

bool decode_to_srgb_rgba8(const std::string& path, std::vector<uint8_t>& rgba, unsigned* width,
                          unsigned* height, std::string* error) {
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  if (!open_frame(path, factory, frame, error)) return false;
  UINT w = 0;
  UINT h = 0;
  HRESULT hr = frame->GetSize(&w, &h);
  if (FAILED(hr) || w < 1 || h < 1) {
    if (error) *error = "Invalid SDR source extent";
    return false;
  }
  std::vector<uint8_t> file;
  if (load_binary_file(path, &file, error) && file.size() >= 2 && file[0] == 0xff && file[1] == 0xd8) {
    unsigned dw = 0;
    unsigned dh = 0;
    if (!decode_jpeg_rgba8(file, rgba, &dw, &dh, error)) return false;
    if (frame_is_display_p3(frame.Get())) {
      display_p3_rgba8888_to_srgb(rgba.data(), static_cast<size_t>(dw) * dh);
    }
    if (width) *width = dw;
    if (height) *height = dh;
    return true;
  }
  Microsoft::WRL::ComPtr<IWICBitmapSource> decoded;
  if (!convert_frame_to_format(factory.Get(), frame.Get(), GUID_WICPixelFormat24bppBGR, decoded,
                               error)) {
    return false;
  }
  if (!copy_bgr24(decoded.Get(), nullptr, static_cast<unsigned>(w), static_cast<unsigned>(h), rgba,
                  error)) {
    return false;
  }
  if (frame_is_display_p3(frame.Get())) {
    display_p3_rgba8888_to_srgb(rgba.data(), static_cast<size_t>(w) * h);
  }
  if (width) *width = static_cast<unsigned>(w);
  if (height) *height = static_cast<unsigned>(h);
  return true;
}

bool encode_jpeg_from_rgba8(const uint8_t* rgba, unsigned width, unsigned height, int quality,
                            std::vector<uint8_t>* jpeg, std::string* error) {
  if (!rgba || !jpeg || width < 1 || height < 1) {
    if (error) *error = "invalid JPEG encode arguments";
    return false;
  }
  jpeg_compress_struct cinfo{};
  JpegError jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;
  unsigned char* outbuf = nullptr;
  unsigned long outsize = 0;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_compress(&cinfo);
    if (outbuf) free(outbuf);
    if (error) *error = "libjpeg failed to compress JPEG";
    return false;
  }
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &outbuf, &outsize);
  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, std::clamp(quality, 1, 100), TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  std::vector<uint8_t> row(static_cast<size_t>(width) * 3u);
  while (cinfo.next_scanline < cinfo.image_height) {
    const uint8_t* src = rgba + static_cast<size_t>(cinfo.next_scanline) * width * 4u;
    for (unsigned x = 0; x < width; ++x) {
      row[x * 3u] = src[x * 4u];
      row[x * 3u + 1] = src[x * 4u + 1];
      row[x * 3u + 2] = src[x * 4u + 2];
    }
    JSAMPROW rows[1] = {row.data()};
    jpeg_write_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  if (!outbuf || outsize == 0) {
    if (error) *error = "libjpeg produced an empty JPEG";
    return false;
  }
  jpeg->assign(outbuf, outbuf + outsize);
  free(outbuf);
  return true;
}

}  // namespace wic
}  // namespace uhdr_repack

#endif
