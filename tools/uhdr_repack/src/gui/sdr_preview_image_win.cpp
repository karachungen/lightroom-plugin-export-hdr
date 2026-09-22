#ifdef _WIN32

#include "gui/sdr_preview_image.h"

#include "wic_utils.h"

#include <QImage>

#include <iostream>
#include <vector>

#include <windows.h>
#include <wincodec.h>

namespace uhdr_repack {
namespace {

bool frame_size(IWICBitmapFrameDecode* frame, int* width, int* height, std::string* error) {
  UINT w = 0;
  UINT h = 0;
  const HRESULT hr = frame->GetSize(&w, &h);
  if (FAILED(hr) || w == 0 || h == 0) {
    if (error) *error = "WIC image has no pixel size";
    return false;
  }
  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  return true;
}

struct ComApartment {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ~ComApartment() {
    if (hr == S_OK || hr == S_FALSE) CoUninitialize();
  }
};

bool open_frame(const std::string& path, Microsoft::WRL::ComPtr<IWICImagingFactory>& factory,
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode>& frame, std::string* error) {
  const ComApartment com;
  if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) {
    if (error) *error = "COM initialization failed";
    return false;
  }
  factory = wic::create_factory();
  if (!factory) {
    if (error) *error = "WIC factory failed";
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
  const std::wstring wide = wic::utf8_to_wide(path);
  HRESULT hr = factory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, &decoder);
  if (FAILED(hr)) {
    if (error) *error = "WIC could not open: " + path;
    return false;
  }
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) {
    if (error) *error = "WIC could not read a frame: " + path;
    return false;
  }
  return true;
}

}  // namespace

bool sdr_preview_size(const std::string& path, int* width, int* height, std::string* error) {
  if (!width || !height) {
    if (error) *error = "invalid arguments";
    return false;
  }
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  if (!open_frame(path, factory, frame, error)) return false;
  return frame_size(frame.Get(), width, height, error);
}

bool load_sdr_preview_image(const std::string& path, QImage* out, std::string* error) {
  if (!out) {
    if (error) *error = "invalid arguments";
    return false;
  }
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  if (!open_frame(path, factory, frame, error)) return false;
  int w = 0;
  int h = 0;
  if (!frame_size(frame.Get(), &w, &h, error)) return false;

  Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
  HRESULT hr = factory->CreateFormatConverter(&converter);
  if (FAILED(hr)) {
    if (error) *error = "WIC CreateFormatConverter failed";
    return false;
  }
  hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                             nullptr, 0.f, WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    if (error) *error = "WIC format conversion failed: " + path;
    return false;
  }

  QImage rgba(w, h, QImage::Format_RGBA8888);
  rgba.fill(0);
  const UINT stride = static_cast<UINT>(rgba.bytesPerLine());
  const UINT bytes = stride * static_cast<UINT>(h);
  hr = converter->CopyPixels(nullptr, stride, bytes, rgba.bits());
  if (FAILED(hr)) {
    if (error) *error = "WIC CopyPixels failed: " + path;
    return false;
  }
  *out = std::move(rgba);
  return true;
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

}  // namespace uhdr_repack

#endif
