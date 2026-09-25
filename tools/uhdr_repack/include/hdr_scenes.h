#pragma once

#include <string>

namespace uhdr_repack {

/** Write sunset/neon/pastel HDR TIFF + SDR JPEG pairs (1080x1350) into dir. */
int write_hdr_scenes_main(const std::string& dir);

}  // namespace uhdr_repack
