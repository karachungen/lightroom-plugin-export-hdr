#ifdef _WIN32

#include "gainmap_compute.h"

#include "color_primaries.h"
#include "gainmap_loader.h"
#include "half_float.h"
#include "path_io.h"
#include "tiff_input.h"
#include "wic_utils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

namespace uhdr_repack {

namespace {

float srgb_byte_to_linear(float value) {
  value /= 255.0f;
  return value <= 0.04045f ? value / 12.92f
                            : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float luminance_bt709(float r, float g, float b) {
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

bool load_sdr_linear_rgba(const std::string& path, unsigned master_w, unsigned master_h,
                          unsigned out_w, unsigned out_h, unsigned crop_x, unsigned crop_y,
                          std::vector<float>* rgba, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!wic::decode_scale_crop_to_rgba8(path, master_w, master_h, out_w, out_h, crop_x, crop_y,
                                       bytes, error))
    return false;
  rgba->resize(static_cast<size_t>(out_w) * out_h * 4u);
  for (size_t i = 0; i < rgba->size(); i += 4) {
    (*rgba)[i] = srgb_byte_to_linear(bytes[i]);
    (*rgba)[i + 1] = srgb_byte_to_linear(bytes[i + 1]);
    (*rgba)[i + 2] = srgb_byte_to_linear(bytes[i + 2]);
    (*rgba)[i + 3] = 1.0f;
  }
  return true;
}

bool hdr_half_to_linear(const uhdr_raw_image_t& hdr, std::vector<float>* rgba) {
  if (hdr.fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || !hdr.planes[UHDR_PLANE_PACKED]) return false;
  const size_t values = static_cast<size_t>(hdr.w) * hdr.h * 4u;
  rgba->resize(values);
  const auto* half = static_cast<const uint16_t*>(hdr.planes[UHDR_PLANE_PACKED]);
  for (size_t pixel = 0; pixel < static_cast<size_t>(hdr.w) * hdr.h; ++pixel) {
    const size_t i = pixel * 4u;
    const LinearRgb rec709 = rec2020_to_linear_srgb(
        {half_to_float(half[i]), half_to_float(half[i + 1]), half_to_float(half[i + 2])});
    (*rgba)[i] = rec709.r;
    (*rgba)[i + 1] = rec709.g;
    (*rgba)[i + 2] = rec709.b;
    (*rgba)[i + 3] = half_to_float(half[i + 3]);
  }
  return true;
}

}  // namespace

bool compute_auto_gainmap(const std::string& sdr_path, const std::string& hdr_tiff_path,
                          std::vector<float>* gain, int* width, int* height, std::string* error) {
  if (!gain || !width || !height) {
    if (error) *error = "null output";
    return false;
  }
  unsigned w = 0;
  unsigned h = 0;
  if (!probe_hdr_tiff_even_size(hdr_tiff_path, &w, &h, error)) return false;
  RawImageHolder hdr;
  if (!load_hdr_tiff_raw(hdr_tiff_path, &hdr, error)) return false;
  std::vector<float> sdr_linear;
  std::vector<float> hdr_linear;
  if (!load_sdr_linear_rgba(sdr_path, w, h, w, h, 0, 0, &sdr_linear, error) ||
      !hdr_half_to_linear(hdr.ref(), &hdr_linear)) {
    if (error && error->empty()) *error = "Could not prepare gain-map source buffers";
    return false;
  }
  gain->resize(static_cast<size_t>(w) * h);
  for (size_t pixel = 0; pixel < gain->size(); ++pixel) {
    const size_t i = pixel * 4u;
    const float sdr_l =
        luminance_bt709(sdr_linear[i], sdr_linear[i + 1], sdr_linear[i + 2]);
    const float hdr_l =
        luminance_bt709(hdr_linear[i], hdr_linear[i + 1], hdr_linear[i + 2]);
    (*gain)[pixel] = std::clamp(hdr_l / std::max(sdr_l, 1e-4f), 1.0f, 1000.0f);
  }
  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  return true;
}

bool write_gainmap_raw_file(const std::string& path, const std::vector<float>& gain, int width,
                            int height, std::string* error) {
  if (width <= 0 || height <= 0 ||
      gain.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
    if (error) *error = "gain map size mismatch";
    return false;
  }
  std::ofstream output = open_output_binary(path);
  if (!output) {
    if (error) *error = "Could not write gain map: " + path;
    return false;
  }
  const int header[2] = {width, height};
  output.write(reinterpret_cast<const char*>(header), sizeof(header));
  output.write(reinterpret_cast<const char*>(gain.data()),
               static_cast<std::streamsize>(gain.size() * sizeof(float)));
  return true;
}

bool apply_gainmap_to_hdr(RawImageHolder* hdr, const std::string& sdr_path,
                          const std::string& gainmap_path, std::string* error,
                          unsigned master_width, unsigned master_height, const CropRect* crop) {
  if (!hdr || hdr->ref().fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat ||
      !hdr->ref().planes[UHDR_PLANE_PACKED]) {
    if (error) *error = "HDR must be half-float RGBA";
    return false;
  }
  uhdr_raw_image_t& image = hdr->ref();
  std::vector<float> gain;
  int gain_w = 0;
  int gain_h = 0;
  if (!load_gainmap_raw_file(gainmap_path, &gain, &gain_w, &gain_h, error)) return false;
  const unsigned master_w = master_width != 0 ? master_width : image.w;
  const unsigned master_h = master_height != 0 ? master_height : image.h;
  const unsigned ox = crop ? crop->x : 0;
  const unsigned oy = crop ? crop->y : 0;
  if (gain_w != static_cast<int>(master_w) || gain_h != static_cast<int>(master_h)) {
    if (error) *error = "Gain map dimensions do not match HDR image";
    return false;
  }
  if (ox + image.w > master_w || oy + image.h > master_h) {
    if (error) *error = "Gain map crop exceeds master bounds";
    return false;
  }
  std::vector<float> sdr_linear;
  if (!load_sdr_linear_rgba(sdr_path, master_w, master_h, image.w, image.h, ox, oy, &sdr_linear,
                            error))
    return false;
  auto* half = static_cast<uint16_t*>(image.planes[UHDR_PLANE_PACKED]);
  for (unsigned y = 0; y < image.h; ++y) {
    for (unsigned x = 0; x < image.w; ++x) {
      const size_t pi = static_cast<size_t>(y) * image.w + x;
      const size_t gi = static_cast<size_t>(oy + y) * static_cast<size_t>(gain_w) + (ox + x);
      const size_t i = pi * 4u;
      const float g = gain[gi];
      const LinearRgb rec2020 = linear_srgb_to_rec2020(
          {sdr_linear[i] * g, sdr_linear[i + 1] * g, sdr_linear[i + 2] * g});
      half[i] = float_to_half(rec2020.r);
      half[i + 1] = float_to_half(rec2020.g);
      half[i + 2] = float_to_half(rec2020.b);
      half[i + 3] = float_to_half(1.0f);
    }
  }
  return true;
}

}  // namespace uhdr_repack

#endif
