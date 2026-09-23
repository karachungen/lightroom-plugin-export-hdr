#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gui/sdr_preview_image.h"

#include "wic_utils.h"

#include <QByteArray>
#include <QImage>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

#include <windows.h>
#include <oleauto.h>
#include <propidl.h>
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

namespace {

bool copy_srgb_preview(const std::string& path, QImage* out, std::string* error) {
  std::vector<uint8_t> bytes;
  unsigned w = 0;
  unsigned h = 0;
  if (!wic::decode_to_srgb_rgba8(path, bytes, &w, &h, error) || w < 1 || h < 1) {
    return false;
  }
  QImage rgba(static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888);
  rgba.fill(0);
  if (static_cast<unsigned>(rgba.bytesPerLine()) == w * 4u) {
    std::memcpy(rgba.bits(), bytes.data(), bytes.size());
  } else {
    for (unsigned y = 0; y < h; ++y) {
      std::memcpy(rgba.scanLine(static_cast<int>(y)),
                  bytes.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u,
                  static_cast<size_t>(w) * 4u);
    }
  }
  *out = std::move(rgba);
  return true;
}

}  // namespace

bool copy_wic_source(IWICImagingFactory* factory, IWICBitmapSource* source, int w, int h,
                     const std::string& path, QImage* out, std::string* error) {
  Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
  HRESULT hr = factory->CreateFormatConverter(&converter);
  if (FAILED(hr)) {
    if (error) *error = "WIC CreateFormatConverter failed";
    return false;
  }
  hr = converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                             0.f, WICBitmapPaletteTypeCustom);
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

bool scale_wic_source(IWICImagingFactory* factory, IWICBitmapSource* source, int dst_w, int dst_h,
                      const std::string& path, QImage* out, std::string* error) {
  Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
  HRESULT hr = factory->CreateBitmapScaler(&scaler);
  if (FAILED(hr)) {
    if (error) *error = "WIC CreateBitmapScaler failed";
    return false;
  }
  hr = scaler->Initialize(source, static_cast<UINT>(dst_w), static_cast<UINT>(dst_h),
                          WICBitmapInterpolationModeFant);
  if (FAILED(hr)) {
    if (error) *error = "WIC scaler Initialize failed: " + path;
    return false;
  }
  return copy_wic_source(factory, scaler.Get(), dst_w, dst_h, path, out, error);
}

bool jpeg_native_scale(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame, int src_w,
                       int src_h, int max_edge, const std::string& path, QImage* out,
                       std::string* error) {
  Microsoft::WRL::ComPtr<IWICBitmapSourceTransform> transform;
  if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&transform)))) return false;

  UINT rw = static_cast<UINT>(std::max(1, src_w));
  UINT rh = static_cast<UINT>(std::max(1, src_h));
  int fit_w = src_w;
  int fit_h = src_h;
  if (!fit_preview_long_edge(src_w, src_h, max_edge, &fit_w, &fit_h)) return false;
  rw = static_cast<UINT>(fit_w);
  rh = static_cast<UINT>(fit_h);
  if (FAILED(transform->GetClosestSize(&rw, &rh)) || rw < 1 || rh < 1) return false;
  if (rw >= static_cast<UINT>(src_w) && rh >= static_cast<UINT>(src_h)) return false;

  QImage native(static_cast<int>(rw), static_cast<int>(rh), QImage::Format_RGBA8888);
  native.fill(0);
  const UINT stride = static_cast<UINT>(native.bytesPerLine());
  const UINT bytes = stride * rh;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
  const HRESULT hr = transform->CopyPixels(nullptr, rw, rh, &format, WICBitmapTransformRotate0,
                                           stride, bytes, native.bits());
  if (FAILED(hr)) return false;

  const int long_edge = std::max(static_cast<int>(rw), static_cast<int>(rh));
  if (long_edge <= max_edge) {
    *out = std::move(native);
    return true;
  }

  Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
  const HRESULT created = factory->CreateBitmapFromMemory(
      rw, rh, GUID_WICPixelFormat32bppRGBA, stride, bytes, native.bits(), &bitmap);
  if (FAILED(created)) {
    if (error) *error = "WIC CreateBitmapFromMemory failed: " + path;
    return false;
  }
  return scale_wic_source(factory, bitmap.Get(), fit_w, fit_h, path, out, error);
}

bool load_sdr_preview_image(const std::string& path, QImage* out, std::string* error, int max_edge) {
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

  int dst_w = w;
  int dst_h = h;
  const bool shrink = max_edge >= 2 && fit_preview_long_edge(w, h, max_edge, &dst_w, &dst_h) &&
                      (dst_w < w || dst_h < h);
  if (!shrink) {
    return copy_srgb_preview(path, out, error);
  }
  if (jpeg_native_scale(factory.Get(), frame.Get(), w, h, max_edge, path, out, error)) {
    return true;
  }
  return scale_wic_source(factory.Get(), frame.Get(), dst_w, dst_h, path, out, error);
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
  const ComApartment com;
  if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) {
    if (error) *error = "COM initialization failed";
    return false;
  }
  const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
  auto factory = wic::create_factory();
  if (!factory) {
    if (error) *error = "WIC factory failed";
    return false;
  }

  Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
  const UINT w = static_cast<UINT>(rgba.width());
  const UINT h = static_cast<UINT>(rgba.height());
  const UINT stride = static_cast<UINT>(rgba.bytesPerLine());
  HRESULT hr = factory->CreateBitmapFromMemory(
      w, h, GUID_WICPixelFormat32bppRGBA, stride, stride * h,
      const_cast<BYTE*>(rgba.constBits()), &bitmap);
  if (FAILED(hr)) {
    if (error) *error = "WIC could not wrap preview pixels";
    return false;
  }

  Microsoft::WRL::ComPtr<IStream> stream;
  hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  if (FAILED(hr)) {
    if (error) *error = "Could not create a JPEG stream";
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
  if (FAILED(hr)) {
    if (error) *error = "WIC JPEG encoder failed";
    return false;
  }
  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr)) {
    if (error) *error = "WIC JPEG encoder init failed";
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
  Microsoft::WRL::ComPtr<IPropertyBag2> bag;
  hr = encoder->CreateNewFrame(&frame, &bag);
  if (FAILED(hr)) {
    if (error) *error = "WIC JPEG frame failed";
    return false;
  }
  if (bag) {
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = static_cast<float>(std::clamp(quality, 1, 100)) / 100.f;
    bag->Write(1, &option, &value);
    VariantClear(&value);
  }
  hr = frame->Initialize(bag.Get());
  if (FAILED(hr)) {
    if (error) *error = "WIC JPEG frame init failed";
    return false;
  }
  hr = frame->SetSize(w, h);
  if (FAILED(hr)) {
    if (error) *error = "WIC JPEG SetSize failed";
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  hr = frame->SetPixelFormat(&format);
  if (FAILED(hr)) {
    if (error) *error = "WIC JPEG SetPixelFormat failed";
    return false;
  }
  hr = frame->WriteSource(bitmap.Get(), nullptr);
  if (FAILED(hr)) {
    if (error) *error = "WIC JPEG WriteSource failed";
    return false;
  }
  hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  if (FAILED(hr)) {
    if (error) *error = "WIC JPEG commit failed";
    return false;
  }

  STATSTG stat{};
  hr = stream->Stat(&stat, STATFLAG_NONAME);
  if (FAILED(hr) || stat.cbSize.QuadPart <= 0) {
    if (error) *error = "WIC JPEG stream is empty";
    return false;
  }
  LARGE_INTEGER zero{};
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  const auto nbytes = static_cast<ULONG>(stat.cbSize.QuadPart);
  out->resize(nbytes);
  ULONG read = 0;
  hr = stream->Read(out->data(), nbytes, &read);
  if (FAILED(hr) || read == 0) {
    out->clear();
    if (error) *error = "WIC JPEG read failed";
    return false;
  }
  out->resize(read);
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

#endif
