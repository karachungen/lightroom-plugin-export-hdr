#ifdef _WIN32

#include "icc_profile.h"
#include "wic_utils.h"

#include <cstring>
#include <string>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

namespace uhdr_repack {
namespace {

bool open_frame(const std::string& path, Microsoft::WRL::ComPtr<IWICImagingFactory>& factory,
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode>& frame, std::string* error) {
  factory = wic::create_factory();
  if (!factory) {
    if (error) *error = "WIC factory unavailable";
    return false;
  }
  const std::wstring wpath = wic::utf8_to_wide(path);
  Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
  HRESULT hr = factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnLoad, &decoder);
  if (FAILED(hr) || !decoder) {
    if (error) *error = "WIC could not open " + path;
    return false;
  }
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr) || !frame) {
    if (error) *error = "WIC could not read a frame from " + path;
    return false;
  }
  return true;
}

bool copy_profile(IWICBitmapFrameDecode* frame, std::vector<uint8_t>* profile, std::string* error) {
  UINT count = 0;
  frame->GetColorContexts(0, nullptr, &count);
  if (count == 0) {
    if (error) *error = "image has no embedded color profile";
    return false;
  }
  IWICColorContext* raw = nullptr;
  UINT actual = 0;
  if (FAILED(frame->GetColorContexts(1, &raw, &actual)) || actual == 0 || !raw) {
    if (raw) raw->Release();
    if (error) *error = "WIC could not read the embedded color profile";
    return false;
  }
  Microsoft::WRL::ComPtr<IWICColorContext> ctx;
  ctx.Attach(raw);
  UINT bytes = 0;
  if (FAILED(ctx->GetProfileBytes(0, nullptr, &bytes)) || bytes < 128) {
    if (error) *error = "WIC returned an empty color profile";
    return false;
  }
  profile->assign(bytes, 0);
  UINT got = 0;
  if (FAILED(ctx->GetProfileBytes(bytes, profile->data(), &got)) || got < 128) {
    if (error) *error = "WIC could not copy the color profile";
    return false;
  }
  profile->resize(got);
  return true;
}

bool copy_float_rgb(IWICBitmapSource* source, UINT width, UINT height, bool rgbx,
                    std::vector<float>* rgb, std::string* error) {
  const UINT bpp = rgbx ? 16u : 12u;
  const UINT stride = width * bpp;
  std::vector<uint8_t> raw(static_cast<size_t>(stride) * height);
  const HRESULT hr = source->CopyPixels(nullptr, stride, static_cast<UINT>(raw.size()), raw.data());
  if (FAILED(hr)) {
    if (error) *error = "WIC CopyPixels failed";
    return false;
  }
  rgb->assign(static_cast<size_t>(width) * height * 3u, 0.0f);
  for (UINT y = 0; y < height; ++y) {
    for (UINT x = 0; x < width; ++x) {
      const float* src = reinterpret_cast<const float*>(raw.data() + static_cast<size_t>(y) * stride +
                                                        static_cast<size_t>(x) * bpp);
      float* dst = rgb->data() + (static_cast<size_t>(y) * width + x) * 3u;
      dst[0] = src[0];
      dst[1] = src[1];
      dst[2] = src[2];
    }
  }
  return true;
}

}  // namespace

bool icc_accepted_by_os(const uint8_t* data, std::size_t size, std::string* error) {
  if (!data || size < 128) {
    if (error) *error = "ICC profile is empty";
    return false;
  }
  auto factory = wic::create_factory();
  if (!factory) {
    if (error) *error = "WIC factory unavailable";
    return false;
  }
  Microsoft::WRL::ComPtr<IWICColorContext> src;
  HRESULT hr = factory->CreateColorContext(&src);
  if (FAILED(hr) || !src) {
    if (error) *error = "WIC could not create a color context";
    return false;
  }
  hr = src->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<UINT>(size));
  if (FAILED(hr)) {
    if (error) *error = "WIC rejected the ICC profile";
    return false;
  }

  BYTE pixel[3] = {180, 40, 40};
  Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
  hr = factory->CreateBitmapFromMemory(1, 1, GUID_WICPixelFormat24bppRGB, 3, 3, pixel, &bitmap);
  if (FAILED(hr) || !bitmap) {
    if (error) *error = "WIC could not build a color-transform source";
    return false;
  }
  Microsoft::WRL::ComPtr<IWICColorContext> dst;
  hr = factory->CreateColorContext(&dst);
  if (FAILED(hr) || FAILED(dst->InitializeFromExifColorSpace(1))) {
    if (error) *error = "WIC could not create an sRGB destination profile";
    return false;
  }
  Microsoft::WRL::ComPtr<IWICColorTransform> transform;
  hr = factory->CreateColorTransformer(&transform);
  if (FAILED(hr) || !transform) {
    if (error) *error = "WIC could not create a color transform";
    return false;
  }
  hr = transform->Initialize(bitmap.Get(), src.Get(), dst.Get(), GUID_WICPixelFormat24bppRGB);
  if (FAILED(hr)) {
    if (error) *error = "WIC could not color-transform with the ICC profile";
    return false;
  }
  return true;
}

bool image_has_color_profile(const std::string& path, const char* description_substring,
                             std::string* error) {
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  if (!open_frame(path, factory, frame, error)) return false;
  std::vector<uint8_t> profile;
  if (!copy_profile(frame.Get(), &profile, error)) return false;
  const std::string hay(reinterpret_cast<const char*>(profile.data()), profile.size());
  if (!description_substring || hay.find(description_substring) == std::string::npos) {
    if (error) *error = "embedded profile is missing " + std::string(description_substring ? description_substring : "");
    return false;
  }
  return true;
}

bool read_tiff_float_rgb_os(const std::string& path, std::vector<float>* rgb, int* width, int* height,
                            std::string* error) {
  if (!rgb || !width || !height) {
    if (error) *error = "internal: null TIFF output";
    return false;
  }
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  if (!open_frame(path, factory, frame, error)) return false;
  UINT w = 0;
  UINT h = 0;
  if (FAILED(frame->GetSize(&w, &h)) || w < 2 || h < 2) {
    if (error) *error = "WIC reported an invalid TIFF size";
    return false;
  }
  WICPixelFormatGUID fmt{};
  frame->GetPixelFormat(&fmt);
  if (IsEqualGUID(fmt, GUID_WICPixelFormat96bppRGBFloat)) {
    if (!copy_float_rgb(frame.Get(), w, h, false, rgb, error)) return false;
  } else if (IsEqualGUID(fmt, GUID_WICPixelFormat128bppRGBFloat)) {
    if (!copy_float_rgb(frame.Get(), w, h, true, rgb, error)) return false;
  } else {
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) {
      if (error) *error = "WIC could not convert the TIFF to float";
      return false;
    }
    const HRESULT hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat128bppRGBFloat,
                                             WICBitmapDitherTypeNone, nullptr, 0.f,
                                             WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
      if (error) *error = "WIC could not convert the TIFF to float RGB";
      return false;
    }
    if (!copy_float_rgb(converter.Get(), w, h, true, rgb, error)) return false;
  }
  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  return true;
}

}  // namespace uhdr_repack

#endif
