#pragma once

#include <string>
#include <vector>

namespace uhdr_repack {

struct CropRect {
  unsigned x = 0;
  unsigned y = 0;
  unsigned w = 0;
  unsigned h = 0;
};

enum class SliceAspect { kNone, k1x1, k4x5 };

/** Parse --slice-aspect value; returns kNone for empty / "none". */
bool parse_slice_aspect(const std::string& s, SliceAspect* out);

/** Label for output filenames, e.g. "1x1" or "4x5". */
const char* slice_aspect_label(SliceAspect aspect);

/**
 * Plan centered horizontal full-height slices on an even-normalized master W×H.
 * tileW = H (1:1) or even floor(H*4/5) (4:5); count = floor(W/tileW); startX centers the row.
 */
bool compute_slices(unsigned master_w, unsigned master_h, SliceAspect aspect,
                    std::vector<CropRect>* slices, std::string* error);

/** e.g. /path/photo.jpg + 1x1 + 1 → /path/photo_1x1_01.jpg */
std::string make_slice_output_path(const std::string& out_path, SliceAspect aspect, unsigned index);

}  // namespace uhdr_repack
