#include "uhdr_encode.h"

#include "gainmap_metadata.h"

#include <ultrahdr_api.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace uhdr_repack {

bool encode_ultra_hdr_jpeg(const RawImageHolder& hdr_holder, const RawImageHolder& sdr_holder,
                           const EncodeOptions& opt, const std::string& output_path,
                           std::string* error) {
  const uhdr_raw_image_t* hdr = hdr_holder.get();
  const uhdr_raw_image_t* sdr = sdr_holder.get();
  if (!hdr || !sdr || hdr->w != sdr->w || hdr->h != sdr->h) {
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
  uhdr_raw_image_t sdr_mut = *sdr;

  uhdr_error_info_t st = uhdr_enc_set_raw_image(enc, &hdr_mut, UHDR_HDR_IMG);
  if (st.error_code != UHDR_CODEC_OK) {
    uhdr_release_encoder(enc);
    if (error) *error = std::string("uhdr_enc_set_raw_image HDR: ") + st.detail;
    return false;
  }

  st = uhdr_enc_set_raw_image(enc, &sdr_mut, UHDR_SDR_IMG);
  if (st.error_code != UHDR_CODEC_OK) {
    uhdr_release_encoder(enc);
    if (error) *error = std::string("uhdr_enc_set_raw_image SDR: ") + st.detail;
    return false;
  }

  uhdr_enc_set_quality(enc, opt.base_quality, UHDR_BASE_IMG);
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

  std::ofstream ofs(output_path, std::ios::binary);
  if (!ofs) {
    uhdr_release_encoder(enc);
    if (error) *error = "Could not open output file: " + output_path;
    return false;
  }
  ofs.write(static_cast<const char*>(out->data), static_cast<std::streamsize>(out->data_sz));
  ofs.close();

  uhdr_release_encoder(enc);
  return true;
}

}  // namespace uhdr_repack
