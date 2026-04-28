#pragma once

#include <ultrahdr_api.h>

namespace uhdr_repack {

struct EncodeOptions;

void apply_gainmap_metadata_policy(uhdr_codec_private_t* enc, const EncodeOptions& opt);

}  // namespace uhdr_repack
