#include "gui/ultrahdr_preview_service.h"

#include "path_io.h"

#include <ultrahdr_api.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <vector>

namespace uhdr_repack {

namespace {

struct DecoderDeleter {
  void operator()(uhdr_codec_private_t* decoder) const {
    if (decoder) uhdr_release_decoder(decoder);
  }
};

bool codec_ok(const uhdr_error_info_t& status, const char* operation, std::string* error) {
  if (status.error_code == UHDR_CODEC_OK) return true;
  if (error) {
    *error = operation;
    if (status.has_detail) {
      *error += ": ";
      *error += status.detail;
    }
  }
  return false;
}

int jpeg_sof_components(const uint8_t* data, size_t len) {
  size_t i = 0;
  while (i + 10 < len) {
    if (data[i] != 0xff) {
      ++i;
      continue;
    }
    const uint8_t marker = data[i + 1];
    if (marker >= 0xc0 && marker <= 0xc3) {
      return static_cast<int>(data[i + 9]);
    }
    if (marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) {
      i += 2;
      continue;
    }
    if (i + 4 > len) break;
    const uint16_t seglen = static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
    if (seglen < 2) break;
    i += 2u + seglen;
  }
  return 0;
}

}  // namespace

bool decode_ultrahdr_preview(const std::string& path, float display_boost, DecodedHdrFrame* out,
                             std::string* error) {
  if (!out) {
    if (error) *error = "decode output is null";
    return false;
  }
  *out = {};

  std::ifstream input = open_input_binary(path);
  if (!input) {
    if (error) *error = "Could not open Ultra HDR preview: " + path;
    return false;
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    if (error) *error = "Ultra HDR preview file is empty";
    return false;
  }

  std::unique_ptr<uhdr_codec_private_t, DecoderDeleter> decoder(uhdr_create_decoder());
  if (!decoder) {
    if (error) *error = "uhdr_create_decoder failed";
    return false;
  }

  uhdr_compressed_image_t compressed{};
  compressed.data = bytes.data();
  compressed.data_sz = bytes.size();
  compressed.capacity = bytes.size();
  compressed.cg = UHDR_CG_UNSPECIFIED;
  compressed.ct = UHDR_CT_UNSPECIFIED;
  compressed.range = UHDR_CR_UNSPECIFIED;

  if (!codec_ok(uhdr_dec_set_image(decoder.get(), &compressed), "uhdr_dec_set_image", error) ||
      !codec_ok(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat),
                "uhdr_dec_set_out_img_format", error) ||
      !codec_ok(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR),
                "uhdr_dec_set_out_color_transfer", error) ||
      !codec_ok(uhdr_dec_set_out_max_display_boost(decoder.get(), std::max(1.0f, display_boost)),
                "uhdr_dec_set_out_max_display_boost", error) ||
      !codec_ok(uhdr_decode(decoder.get()), "uhdr_decode", error)) {
    return false;
  }

  uhdr_raw_image_t* image = uhdr_get_decoded_image(decoder.get());
  if (!image || !image->planes[UHDR_PLANE_PACKED] ||
      image->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || image->w == 0 || image->h == 0) {
    if (error) *error = "Ultra HDR decoder did not return linear RGBA16F";
    return false;
  }

  out->width = static_cast<int>(image->w);
  out->height = static_cast<int>(image->h);
  out->jpeg_bytes = bytes.size();
  out->display_boost = std::max(1.0f, display_boost);
  out->color_gamut = static_cast<int>(image->cg);
  out->rgba_half.resize(static_cast<size_t>(image->w) * image->h * 4u);
  const auto* src = static_cast<const uint16_t*>(image->planes[UHDR_PLANE_PACKED]);
  const size_t row_values = static_cast<size_t>(image->w) * 4u;
  for (unsigned y = 0; y < image->h; ++y) {
    std::memcpy(out->rgba_half.data() + static_cast<size_t>(y) * row_values,
                src + static_cast<size_t>(y) * image->stride[UHDR_PLANE_PACKED] * 4u,
                row_values * sizeof(uint16_t));
  }

  uhdr_mem_block_t* gainmap = uhdr_dec_get_gainmap_image(decoder.get());
  if (gainmap && gainmap->data && gainmap->data_sz > 0) {
    const auto* jpeg = static_cast<const uint8_t*>(gainmap->data);
    out->gainmap_jpeg.assign(jpeg, jpeg + gainmap->data_sz);
    out->gainmap_is_rgb = jpeg_sof_components(jpeg, gainmap->data_sz) >= 3;
  }
  return true;
}

}  // namespace uhdr_repack
