#pragma once

#include <cstddef>
#include <string>

namespace uhdr_repack {

/** Instagram's upload limit for photos. */
constexpr size_t kInstagramMaxBytes = 8u * 1024u * 1024u;

struct UhdrExpect {
  int width = 0;             // 0 skips the size check
  int height = 0;
  int gainmap_scale = 1;
  size_t max_bytes = 0;      // 0 skips the file-size limit
  std::string single_aspect; // non-empty: fail when numbered slices exist
  bool skip_if_missing = false;
};

/**
 * Fail unless path is an Ultra HDR JPEG that browsers and Instagram read with its colors:
 * MPF, hdrgm XMP, ISO 21496-1 APP2, a Display P3 / sRGB-curve primary ICC, the expected gain-map
 * scale, xmpRights URLs, and the optional size, byte, and single-slice expectations.
 * @return 0 pass, 1 fail, 77 missing with skip_if_missing.
 */
int verify_uhdr_file(const std::string& path, const UhdrExpect& expect);

int verify_uhdr_main(int argc, char** argv);
int check_utf8_path_main(const std::string& hdr, const std::string& sdr, const std::string& out_dir);

/** Return 77 when --hdr-tiff or --base is missing on disk. Otherwise run the CLI encoder. */
int encode_or_skip_main(int argc, char** argv);

}  // namespace uhdr_repack
