#include "uhdr_encode.h"

#include "gainmap_metadata.h"
#include "jpeg_xmp_rights.h"
#include "path_io.h"

#include <ultrahdr_api.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace uhdr_repack {

bool encode_ultra_hdr_jpeg(const RawImageHolder& hdr_holder, const RawImageHolder* sdr_holder,
                           const EncodeOptions& opt, const std::string& output_path,
                           std::string* error) {
  const uhdr_raw_image_t* hdr = hdr_holder.get();
  const bool use_jpeg = !opt.sdr_jpeg.empty();
  const uhdr_raw_image_t* sdr = (!use_jpeg && sdr_holder) ? sdr_holder->get() : nullptr;
  if (!hdr || (!use_jpeg && (!sdr || hdr->w != sdr->w || hdr->h != sdr->h))) {
    if (error) {
      *error = "HDR and SDR must have matching dimensions";
    }
    return false;
  }

  uhdr_codec_private_t* enc = uhdr_create_encoder();
  if (!enc) {
    if (error) *error = "uhdr_create_encoder failed";
    return false;
  }

  uhdr_raw_image_t hdr_mut = *hdr;
  uhdr_error_info_t st = uhdr_enc_set_raw_image(enc, &hdr_mut, UHDR_HDR_IMG);
  if (st.error_code != UHDR_CODEC_OK) {
    uhdr_release_encoder(enc);
    if (error) *error = std::string("uhdr_enc_set_raw_image HDR: ") + st.detail;
    return false;
  }

  uhdr_raw_image_t sdr_mut{};
  uhdr_compressed_image_t jpeg{};
  if (use_jpeg) {
    jpeg.data = const_cast<uint8_t*>(opt.sdr_jpeg.data());
    jpeg.data_sz = opt.sdr_jpeg.size();
    jpeg.capacity = opt.sdr_jpeg.size();
    jpeg.cg = static_cast<uhdr_color_gamut_t>(opt.sdr_jpeg_cg);
    jpeg.ct = UHDR_CT_SRGB;
    jpeg.range = UHDR_CR_FULL_RANGE;
    // UHDR_BASE_IMG is the already-muxed primary for a base+gain-map encode. With an HDR raw
    // image and no gain map, that label makes libultrahdr take the HDR-only path and invent an
    // SDR base. UHDR_SDR_IMG is the SDR rendition the gain map is built against.
    st = uhdr_enc_set_compressed_image(enc, &jpeg, UHDR_SDR_IMG);
    if (st.error_code != UHDR_CODEC_OK) {
      uhdr_release_encoder(enc);
      if (error) *error = std::string("uhdr_enc_set_compressed_image SDR: ") + st.detail;
      return false;
    }
  } else {
    sdr_mut = *sdr;
    st = uhdr_enc_set_raw_image(enc, &sdr_mut, UHDR_SDR_IMG);
    if (st.error_code != UHDR_CODEC_OK) {
      uhdr_release_encoder(enc);
      if (error) *error = std::string("uhdr_enc_set_raw_image SDR: ") + st.detail;
      return false;
    }
    uhdr_enc_set_quality(enc, opt.base_quality, UHDR_BASE_IMG);
  }

  uhdr_enc_set_quality(enc, opt.gainmap_quality, UHDR_GAIN_MAP_IMG);
  uhdr_enc_set_gainmap_scale_factor(enc, opt.gainmap_scale);
  uhdr_enc_set_preset(enc, UHDR_USAGE_BEST_QUALITY);
  uhdr_enc_set_output_format(enc, UHDR_CODEC_JPG);
  apply_gainmap_metadata_policy(enc, opt);

  st = uhdr_encode(enc);
  if (st.error_code != UHDR_CODEC_OK) {
    uhdr_release_encoder(enc);
    if (error) *error = std::string("uhdr_encode: ") + st.detail;
    return false;
  }

  uhdr_compressed_image_t* out = uhdr_get_encoded_stream(enc);
  if (!out || !out->data || out->data_sz == 0) {
    uhdr_release_encoder(enc);
    if (error) *error = "uhdr_get_encoded_stream returned empty";
    return false;
  }

  std::vector<uint8_t> jpeg_out(static_cast<const uint8_t*>(out->data),
                                static_cast<const uint8_t*>(out->data) + out->data_sz);
  if (!inject_sdr_xmp_rights(&jpeg_out, error)) {
    uhdr_release_encoder(enc);
    return false;
  }

  std::ofstream ofs = open_output_binary(output_path);
  if (!ofs) {
    uhdr_release_encoder(enc);
    if (error) *error = "Could not open output file: " + output_path;
    return false;
  }
  ofs.write(reinterpret_cast<const char*>(jpeg_out.data()),
            static_cast<std::streamsize>(jpeg_out.size()));
  ofs.close();

  uhdr_release_encoder(enc);
  return true;
}

}  // namespace uhdr_repack
