#include "compatibility_gainmap.h"

#include "half_float.h"
#include "path_io.h"

#include <ultrahdr_api.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace uhdr_repack {
namespace {

// A scalar ratio is ill-conditioned when Lightroom's 8-bit SDR JPEG clips a shadow to code 0
// while the float HDR rendition still contains sub-black detail. 1e-7 produced 9-14 stop spikes
// from two visually black values. A small, equal black offset keeps the equation reversible at
// full gain, preserves negative gain, and is well below Adobe HDRGM's 1/64 fallback offset.
constexpr float kOffsetSdr = 1.0f / 1024.0f;
constexpr float kOffsetHdr = 1.0f / 1024.0f;
constexpr float kGainmapGamma = 1.0f;
constexpr float kSdrWhiteNits = 203.0f;

struct JpegErrorManager {
  jpeg_error_mgr base;
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX]{};
};

void JpegErrorExit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<JpegErrorManager*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->message);
  longjmp(err->jump, 1);
}

bool CompressJpeg(const uint8_t* pixels, unsigned width, unsigned height, int components,
                  int quality, std::vector<uint8_t>* jpeg, std::string* error) {
  jpeg_compress_struct cinfo{};
  JpegErrorManager jerr{};
  cinfo.err = jpeg_std_error(&jerr.base);
  jerr.base.error_exit = JpegErrorExit;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_compress(&cinfo);
    if (error) *error = std::string("JPEG encode: ") + jerr.message;
    return false;
  }

  jpeg_create_compress(&cinfo);
  unsigned char* output = nullptr;
  unsigned long output_size = 0;
  jpeg_mem_dest(&cinfo, &output, &output_size);
  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = components;
  cinfo.in_color_space = components == 1 ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  const size_t row_stride = static_cast<size_t>(width) * components;
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(pixels + cinfo.next_scanline * row_stride);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg->assign(output, output + output_size);
  std::free(output);
  jpeg_destroy_compress(&cinfo);
  return true;
}

float SrgbToLinear(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value <= 0.04045f ? value / 12.92f
                           : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

void ReadSdrGammaRgb(const uhdr_raw_image_t& sdr, unsigned x, unsigned y, float* r, float* g,
                     float* b) {
  if (sdr.fmt == UHDR_IMG_FMT_32bppRGBA8888) {
    const auto* rgba = static_cast<const uint8_t*>(sdr.planes[UHDR_PLANE_PACKED]);
    const size_t i = (static_cast<size_t>(y) * sdr.stride[UHDR_PLANE_PACKED] + x) * 4;
    *r = rgba[i] / 255.0f;
    *g = rgba[i + 1] / 255.0f;
    *b = rgba[i + 2] / 255.0f;
    return;
  }
  const auto* py = static_cast<const uint8_t*>(sdr.planes[UHDR_PLANE_Y]);
  const auto* pu = static_cast<const uint8_t*>(sdr.planes[UHDR_PLANE_U]);
  const auto* pv = static_cast<const uint8_t*>(sdr.planes[UHDR_PLANE_V]);
  const float yy = py[static_cast<size_t>(y) * sdr.stride[UHDR_PLANE_Y] + x] / 255.0f;
  const float cb = pu[static_cast<size_t>(y / 2) * sdr.stride[UHDR_PLANE_U] + x / 2] / 255.0f -
                   (128.0f / 255.0f);
  const float cr = pv[static_cast<size_t>(y / 2) * sdr.stride[UHDR_PLANE_V] + x / 2] / 255.0f -
                   (128.0f / 255.0f);
  *r = std::clamp(yy + 1.5748f * cr, 0.0f, 1.0f);
  *g = std::clamp(yy - 0.187324f * cb - 0.468124f * cr, 0.0f, 1.0f);
  *b = std::clamp(yy + 1.8556f * cb, 0.0f, 1.0f);
}

float SdrLuminance(const uhdr_raw_image_t& sdr, unsigned x, unsigned y) {
  float r = 0.0f, g = 0.0f, b = 0.0f;
  ReadSdrGammaRgb(sdr, x, y, &r, &g, &b);
  return std::max(0.0f, 0.212639f * SrgbToLinear(r) + 0.715169f * SrgbToLinear(g) +
                            0.072192f * SrgbToLinear(b));
}

float HdrLuminance(const uhdr_raw_image_t& hdr, unsigned x, unsigned y) {
  const auto* rgba = static_cast<const fp16_t*>(hdr.planes[UHDR_PLANE_PACKED]);
  const size_t i = (static_cast<size_t>(y) * hdr.stride[UHDR_PLANE_PACKED] + x) * 4;
  const float r = half_to_float(rgba[i]);
  const float g = half_to_float(rgba[i + 1]);
  const float b = half_to_float(rgba[i + 2]);
  const float luminance = 0.2627f * r + 0.677998f * g + 0.059302f * b;
  return std::isfinite(luminance) ? std::max(0.0f, luminance) : 0.0f;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* bytes, std::string* error) {
  std::ifstream input = open_input_binary(path);
  if (!input) {
    if (error) *error = "Could not read Lightroom SDR JPEG: " + path;
    return false;
  }
  bytes->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (bytes->empty()) {
    if (error) *error = "Lightroom SDR JPEG is empty: " + path;
    return false;
  }
  return true;
}

bool CompressSdrSlice(const uhdr_raw_image_t& sdr, int quality, std::vector<uint8_t>* jpeg,
                      std::string* error) {
  std::vector<uint8_t> rgb(static_cast<size_t>(sdr.w) * sdr.h * 3);
  for (unsigned y = 0; y < sdr.h; ++y) {
    for (unsigned x = 0; x < sdr.w; ++x) {
      float r = 0.0f, g = 0.0f, b = 0.0f;
      ReadSdrGammaRgb(sdr, x, y, &r, &g, &b);
      const size_t i = (static_cast<size_t>(y) * sdr.w + x) * 3;
      rgb[i] = static_cast<uint8_t>(std::lround(r * 255.0f));
      rgb[i + 1] = static_cast<uint8_t>(std::lround(g * 255.0f));
      rgb[i + 2] = static_cast<uint8_t>(std::lround(b * 255.0f));
    }
  }
  return CompressJpeg(rgb.data(), sdr.w, sdr.h, 3, quality, jpeg, error);
}

bool WriteBytes(const std::string& path, const std::vector<uint8_t>& bytes, std::string* error) {
  std::ofstream output = open_output_binary(path);
  if (!output) {
    if (error) *error = "Could not open output file: " + path;
    return false;
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    if (error) *error = "Could not write output file: " + path;
    return false;
  }
  return true;
}

bool CheckStatus(const uhdr_error_info_t& status, const char* operation, std::string* error) {
  if (status.error_code == UHDR_CODEC_OK) return true;
  if (error) {
    *error = std::string(operation) + ": " + (status.has_detail ? status.detail : "failed");
  }
  return false;
}

}  // namespace

bool build_compatibility_gainmap(const RawImageHolder& hdr_holder,
                                 const RawImageHolder& sdr_holder, const EncodeOptions& opt,
                                 CompatibilityGainmap* result, std::string* error) {
  if (!result) {
    if (error) *error = "null compatibility gain map result";
    return false;
  }
  *result = CompatibilityGainmap{};
  const uhdr_raw_image_t* hdr = hdr_holder.get();
  const uhdr_raw_image_t* sdr = sdr_holder.get();
  if (!hdr || !sdr || hdr->w != sdr->w || hdr->h != sdr->h || opt.gainmap_scale < 1) {
    if (error) *error = "HDR and SDR must have matching dimensions and a positive scale";
    return false;
  }
  if (hdr->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || hdr->ct != UHDR_CT_LINEAR ||
      hdr->cg != UHDR_CG_BT_2100 ||
      (sdr->fmt != UHDR_IMG_FMT_12bppYCbCr420 &&
       sdr->fmt != UHDR_IMG_FMT_32bppRGBA8888) ||
      sdr->ct != UHDR_CT_SRGB || sdr->cg != UHDR_CG_BT_709) {
    if (error) *error = "Compatibility scalar expects Linear Rec.2020 HDR and sRGB/BT.709 SDR";
    return false;
  }

  const unsigned scale = static_cast<unsigned>(opt.gainmap_scale);
  result->width = hdr->w / scale;
  result->height = hdr->h / scale;
  if (result->width == 0 || result->height == 0) {
    if (error) *error = "Gain map scale is larger than the image";
    return false;
  }

  result->gains_log2.resize(static_cast<size_t>(result->width) * result->height);
  float min_gain = std::numeric_limits<float>::infinity();
  float max_gain = -std::numeric_limits<float>::infinity();
  for (unsigned map_y = 0; map_y < result->height; ++map_y) {
    for (unsigned map_x = 0; map_x < result->width; ++map_x) {
      double sum = 0.0;
      for (unsigned dy = 0; dy < scale; ++dy) {
        for (unsigned dx = 0; dx < scale; ++dx) {
          const unsigned x = map_x * scale + dx;
          const unsigned y = map_y * scale + dy;
          const float sdr_y = SdrLuminance(*sdr, x, y);
          const float hdr_y = HdrLuminance(*hdr, x, y);
          float gain = std::log2((hdr_y + kOffsetHdr) / (sdr_y + kOffsetSdr));
          if (!std::isfinite(gain)) gain = 0.0f;
          sum += gain;
        }
      }
      const float gain = static_cast<float>(sum / static_cast<double>(scale * scale));
      result->gains_log2[static_cast<size_t>(map_y) * result->width + map_x] = gain;
      min_gain = std::min(min_gain, gain);
      max_gain = std::max(max_gain, gain);
    }
  }

  if (!std::isfinite(min_gain) || !std::isfinite(max_gain)) {
    if (error) *error = "Calculated gain map range is not finite";
    return false;
  }
  if (max_gain - min_gain < 1.0e-6f) {
    min_gain -= 5.0e-7f;
    max_gain += 5.0e-7f;
  }
  result->min_gain_log2 = min_gain;
  result->max_gain_log2 = max_gain;
  result->pixels.resize(result->gains_log2.size());
  const float inverse_range = 1.0f / (max_gain - min_gain);
  for (size_t i = 0; i < result->gains_log2.size(); ++i) {
    const float normalized = std::clamp((result->gains_log2[i] - min_gain) * inverse_range,
                                        0.0f, 1.0f);
    result->pixels[i] = static_cast<uint8_t>(std::lround(normalized * 255.0f));
  }
  if (!CompressJpeg(result->pixels.data(), result->width, result->height, 1,
                    opt.gainmap_quality, &result->jpeg, error)) {
    return false;
  }

  for (int channel = 0; channel < 3; ++channel) {
    result->metadata.min_content_boost[channel] = std::exp2(min_gain);
    result->metadata.max_content_boost[channel] = std::exp2(max_gain);
    result->metadata.gamma[channel] = kGainmapGamma;
    result->metadata.offset_sdr[channel] = kOffsetSdr;
    result->metadata.offset_hdr[channel] = kOffsetHdr;
  }
  result->metadata.hdr_capacity_min = 1.0f;
  result->metadata.hdr_capacity_max =
      std::max(opt.target_display_peak_nits / kSdrWhiteNits, std::nextafter(1.0f, 2.0f));
  result->metadata.use_base_cg = 1;
  return true;
}

bool encode_compatibility_scalar_jpeg(const RawImageHolder& hdr, const RawImageHolder& sdr,
                                      const EncodeOptions& opt, const std::string& base_path,
                                      bool preserve_compressed_base,
                                      const std::string& output_path, std::string* error) {
  CompatibilityGainmap gainmap;
  if (!build_compatibility_gainmap(hdr, sdr, opt, &gainmap, error)) return false;

  std::vector<uint8_t> base_jpeg;
  if (preserve_compressed_base) {
    if (!ReadFile(base_path, &base_jpeg, error)) return false;
  } else if (!CompressSdrSlice(sdr.ref(), opt.base_quality, &base_jpeg, error)) {
    return false;
  }
  if (!opt.gainmap_debug_output_path.empty() &&
      !WriteBytes(opt.gainmap_debug_output_path, gainmap.jpeg, error)) {
    return false;
  }

  uhdr_compressed_image_t base{};
  base.data = base_jpeg.data();
  base.data_sz = base.capacity = base_jpeg.size();
  // Existing ICC is retained by API-4; this fallback lets libultrahdr add sRGB ICC when absent.
  base.cg = UHDR_CG_BT_709;
  base.ct = UHDR_CT_SRGB;
  base.range = UHDR_CR_FULL_RANGE;
  uhdr_compressed_image_t map{};
  map.data = gainmap.jpeg.data();
  map.data_sz = map.capacity = gainmap.jpeg.size();
  map.cg = UHDR_CG_UNSPECIFIED;
  map.ct = UHDR_CT_UNSPECIFIED;
  map.range = UHDR_CR_UNSPECIFIED;

  uhdr_codec_private_t* enc = uhdr_create_encoder();
  if (!enc) {
    if (error) *error = "uhdr_create_encoder failed";
    return false;
  }
  bool ok = CheckStatus(uhdr_enc_set_compressed_image(enc, &base, UHDR_BASE_IMG),
                        "uhdr_enc_set_compressed_image base", error) &&
            CheckStatus(uhdr_enc_set_gainmap_image(enc, &map, &gainmap.metadata),
                        "uhdr_enc_set_gainmap_image", error) &&
            CheckStatus(uhdr_enc_set_output_format(enc, UHDR_CODEC_JPG),
                        "uhdr_enc_set_output_format", error) &&
            CheckStatus(uhdr_encode(enc), "uhdr_encode compatibility scalar", error);
  if (!ok) {
    uhdr_release_encoder(enc);
    return false;
  }
  uhdr_compressed_image_t* encoded = uhdr_get_encoded_stream(enc);
  if (!encoded || !encoded->data || encoded->data_sz == 0) {
    if (error) *error = "uhdr_get_encoded_stream returned empty";
    uhdr_release_encoder(enc);
    return false;
  }
  std::vector<uint8_t> output(static_cast<uint8_t*>(encoded->data),
                              static_cast<uint8_t*>(encoded->data) + encoded->data_sz);
  uhdr_release_encoder(enc);
  if (!WriteBytes(output_path, output, error)) return false;

  std::cout << "Gain map algorithm: compatibility-scalar\n"
            << "Compatibility GainMapMin: " << gainmap.min_gain_log2 << "\n"
            << "Compatibility GainMapMax: " << gainmap.max_gain_log2 << "\n"
            << "Compatibility Gamma: " << kGainmapGamma << "\n"
            << "Compatibility OffsetSDR: " << kOffsetSdr << "\n"
            << "Compatibility OffsetHDR: " << kOffsetHdr << "\n"
            << "Compatibility gain map dimensions: " << gainmap.width << "x" << gainmap.height
            << "\n"
            << "Compatibility gain map JPEG quality: " << opt.gainmap_quality << "\n"
            << "Compatibility gain map scale: " << opt.gainmap_scale << "\n";
  return true;
}

}  // namespace uhdr_repack
