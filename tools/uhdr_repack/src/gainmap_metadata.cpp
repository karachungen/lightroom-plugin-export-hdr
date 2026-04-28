#include "gainmap_metadata.h"

#include "uhdr_encode.h"

#include <ultrahdr_api.h>

namespace uhdr_repack {

void apply_gainmap_metadata_policy(uhdr_codec_private_t* enc, const EncodeOptions& opt) {
  if (!enc) {
    return;
  }
  uhdr_enc_set_min_max_content_boost(enc, opt.min_content_boost, opt.max_content_boost);
  uhdr_enc_set_target_display_peak_brightness(enc, opt.target_display_peak_nits);
  uhdr_enc_set_using_multi_channel_gainmap(enc, opt.monochrome_gainmap ? 0 : 1);
}

}  // namespace uhdr_repack
