#ifdef _WIN32

#include "gui/sdr_preview_image.h"

#include "wic_utils.h"

#include <QByteArray>
#include <QImage>

#include <algorithm>
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
