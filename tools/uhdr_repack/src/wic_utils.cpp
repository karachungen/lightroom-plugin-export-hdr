#ifdef _WIN32

#include "wic_utils.h"

#include <cmath>
#include <cstring>

#include <windows.h>

namespace uhdr_repack {
namespace wic {

namespace {

HRESULT hr_or_error(HRESULT hr, std::string* error, const std::string& msg) {
  if (FAILED(hr) && error) {
    *error = msg + " (HRESULT=" + std::to_string(static_cast<unsigned long>(hr)) + ")";
  }
  return hr;
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

}  // namespace

Microsoft::WRL::ComPtr<IWICImagingFactory> create_factory() {
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
                                std::string* error) {
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
    hr_or_error(hr, error, "WIC could not open SDR base: " + path);
    return false;
  }

  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC GetFrame failed");
    return false;
  }

  UINT src_w = 0;
  UINT src_h = 0;
  hr = frame->GetSize(&src_w, &src_h);
  if (FAILED(hr) || src_w < 1 || src_h < 1) {
    if (error) {
      *error = "Invalid SDR source extent";
    }
    return false;
  }

  Microsoft::WRL::ComPtr<IWICBitmapSource> rgba_source;
  if (!convert_frame_to_format(factory.Get(), frame.Get(), GUID_WICPixelFormat32bppRGBA,
                               rgba_source, error)) {
    return false;
  }

  Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
  hr = factory->CreateBitmapScaler(&scaler);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC CreateBitmapScaler failed");
    return false;
  }

  hr = scaler->Initialize(rgba_source.Get(), master_width, master_height,
                          WICBitmapInterpolationModeHighQualityCubic);
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC scaler Initialize failed");
    return false;
  }

  WICRect crop_rect{};
  crop_rect.X = static_cast<INT>(crop_x);
  crop_rect.Y = static_cast<INT>(crop_y);
  crop_rect.Width = static_cast<INT>(out_w);
  crop_rect.Height = static_cast<INT>(out_h);

  const size_t nbytes = static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 4;
  rgba.assign(nbytes, 0);
  const UINT stride = out_w * 4;
  hr = scaler->CopyPixels(&crop_rect, stride, static_cast<UINT>(nbytes), rgba.data());
  if (FAILED(hr)) {
    hr_or_error(hr, error, "WIC scaled crop CopyPixels failed");
    return false;
  }
  return true;
}

}  // namespace wic
}  // namespace uhdr_repack

#endif
