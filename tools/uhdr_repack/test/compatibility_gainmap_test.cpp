#include "compatibility_gainmap.h"
#include "half_float.h"

#include <ultrahdr_api.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

int JpegComponents(const std::vector<uint8_t>& jpeg) {
  for (size_t i = 0; i + 10 < jpeg.size(); ++i) {
    if (jpeg[i] == 0xff && jpeg[i + 1] >= 0xc0 && jpeg[i + 1] <= 0xc3) {
      return jpeg[i + 9];
    }
  }
  return -1;
}

bool Fail(const char* message) {
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

}  // namespace

int main() {
  constexpr unsigned kWidth = 4;
  constexpr unsigned kHeight = 4;
  uhdr_repack::RawImageHolder hdr;
  uhdr_repack::RawImageHolder sdr;

  auto* rgba8 = static_cast<uint8_t*>(std::malloc(kWidth * kHeight * 4));
  auto* rgba = static_cast<uhdr_repack::fp16_t*>(
      std::malloc(kWidth * kHeight * 4 * sizeof(uhdr_repack::fp16_t)));
  if (!rgba8 || !rgba) return Fail("allocation") ? 0 : 1;
  std::memset(rgba8, 128, kWidth * kHeight * 4);
  for (size_t pixel = 0; pixel < kWidth * kHeight; ++pixel) rgba8[pixel * 4 + 3] = 255;
  for (size_t pixel = 0; pixel < kWidth * kHeight; ++pixel) {
    const float value = pixel < 8 ? 0.01f : 1.0f;
    rgba[pixel * 4] = uhdr_repack::float_to_half(value);
    rgba[pixel * 4 + 1] = uhdr_repack::float_to_half(value);
    rgba[pixel * 4 + 2] = uhdr_repack::float_to_half(value);
    rgba[pixel * 4 + 3] = uhdr_repack::float_to_half(1.0f);
  }
  // Lightroom may quantize a visually black SDR sample to zero while HDR retains tiny detail.
  for (int channel = 0; channel < 3; ++channel) {
    rgba8[5 * 4 + channel] = 0;
    rgba[5 * 4 + channel] = uhdr_repack::float_to_half(0.0005f);
  }
  // Exercise invalid/black HDR input handling without allowing NaN into the map.
  rgba[0] = uhdr_repack::float_to_half(NAN);
  rgba[1] = uhdr_repack::float_to_half(NAN);
  rgba[2] = uhdr_repack::float_to_half(NAN);

  sdr.ref().fmt = UHDR_IMG_FMT_32bppRGBA8888;
  sdr.ref().cg = UHDR_CG_BT_709;
  sdr.ref().ct = UHDR_CT_SRGB;
  sdr.ref().range = UHDR_CR_FULL_RANGE;
  sdr.ref().w = kWidth;
  sdr.ref().h = kHeight;
  sdr.ref().planes[UHDR_PLANE_PACKED] = rgba8;
  sdr.ref().stride[UHDR_PLANE_PACKED] = kWidth;

  hdr.ref().fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
  hdr.ref().cg = UHDR_CG_BT_2100;
  hdr.ref().ct = UHDR_CT_LINEAR;
  hdr.ref().range = UHDR_CR_FULL_RANGE;
  hdr.ref().w = kWidth;
  hdr.ref().h = kHeight;
  hdr.ref().planes[UHDR_PLANE_PACKED] = rgba;
  hdr.ref().stride[UHDR_PLANE_PACKED] = kWidth;

  uhdr_repack::EncodeOptions options;
  options.gainmap_scale = 2;
  options.gainmap_quality = 73;
  options.target_display_peak_nits = 1000.0f;
  uhdr_repack::CompatibilityGainmap result;
  std::string error;
  if (!uhdr_repack::build_compatibility_gainmap(hdr, sdr, options, &result, &error)) {
    std::cerr << "FAIL: " << error << "\n";
    return 1;
  }
  if (result.width != 2 || result.height != 2) return Fail("scale dimensions") ? 0 : 1;
  if (JpegComponents(result.jpeg) != 1) return Fail("gain map JPEG is not grayscale") ? 0 : 1;
  if (!(result.min_gain_log2 < 0.0f)) return Fail("negative GainMapMin lost") ? 0 : 1;
  if (!std::isfinite(result.min_gain_log2) || !std::isfinite(result.max_gain_log2)) {
    return Fail("NaN/Inf gain range") ? 0 : 1;
  }
  for (float gain : result.gains_log2) {
    if (!std::isfinite(gain)) return Fail("NaN/Inf gain pixel") ? 0 : 1;
  }
  const auto range = std::minmax_element(result.gains_log2.begin(), result.gains_log2.end());
  if (std::fabs(*range.first - result.min_gain_log2) > 1.0e-6f ||
      std::fabs(*range.second - result.max_gain_log2) > 1.0e-6f) {
    return Fail("metadata range was not calculated from the downsampled gain map") ? 0 : 1;
  }

  options.gainmap_scale = 1;
  uhdr_repack::CompatibilityGainmap edge_result;
  if (!uhdr_repack::build_compatibility_gainmap(hdr, sdr, options, &edge_result, &error)) {
    std::cerr << "FAIL: " << error << "\n";
    return 1;
  }
  if (edge_result.gains_log2[5] < 0.0f || edge_result.gains_log2[5] > 1.0f) {
    return Fail("quantized SDR black produced a large positive gain") ? 0 : 1;
  }
  if (std::fabs(edge_result.metadata.offset_sdr[0] - 1.0f / 1024.0f) > 1.0e-8f ||
      std::fabs(edge_result.metadata.offset_hdr[0] - 1.0f / 1024.0f) > 1.0e-8f) {
    return Fail("compatibility black offsets differ from encoded gain equation") ? 0 : 1;
  }
  if (std::fabs(std::log2(result.metadata.min_content_boost[0]) - result.min_gain_log2) >
          1.0e-4f ||
      std::fabs(std::log2(result.metadata.max_content_boost[0]) - result.max_gain_log2) >
          1.0e-4f) {
    return Fail("metadata range differs from encoded range") ? 0 : 1;
  }

  uhdr_compressed_image_t base{};
  base.data = result.jpeg.data();
  base.data_sz = base.capacity = result.jpeg.size();
  uhdr_compressed_image_t map = base;
  uhdr_codec_private_t* encoder = uhdr_create_encoder();
  if (!encoder) return Fail("encoder creation") ? 0 : 1;
  const auto base_status = uhdr_enc_set_compressed_image(encoder, &base, UHDR_BASE_IMG);
  const auto map_status = uhdr_enc_set_gainmap_image(encoder, &map, &result.metadata);
  const auto encode_status = uhdr_encode(encoder);
  const bool encoded = base_status.error_code == UHDR_CODEC_OK &&
                       map_status.error_code == UHDR_CODEC_OK &&
                       encode_status.error_code == UHDR_CODEC_OK;
  uhdr_release_encoder(encoder);
  if (!encoded) return Fail("negative GainMapMin metadata rejected by encoder") ? 0 : 1;

  std::cout << "PASS: compatibility scalar grayscale, scale, range, black/NaN safety, metadata\n";
  return 0;
}
