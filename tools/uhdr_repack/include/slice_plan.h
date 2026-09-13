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

enum class SliceAspect { kNone, k1x1, k4x5, k3x4, k191x100 };

/** Instagram carousel hard cap (gallery / carousel posts). */
constexpr unsigned kInstagramGalleryMax = 20;

/** Parse --slice-aspect / Instagram preset; returns kNone for empty / "none". */
bool parse_slice_aspect(const std::string& s, SliceAspect* out);

/** Label for output filenames, e.g. "4x5" or "3x4". */
const char* slice_aspect_label(SliceAspect aspect);

/** Instagram 1080-wide feed size (even). Returns false for kNone. */
bool instagram_output_size(SliceAspect aspect, unsigned* width, unsigned* height);

/**
 * 1× or 2× Instagram feed box (even). Named aspects ignore crop.
 * Original (kNone) is 1080- or 2160-wide with height from the crop. scale must be 1 or 2.
 */
bool instagram_output_size_scaled(SliceAspect aspect, unsigned scale, unsigned crop_w,
                                  unsigned crop_h, unsigned* width, unsigned* height);

/** True if W/H is inside Instagram’s HDR-safe feed range 1.91:1 through 3:4. */
bool instagram_aspect_supported(unsigned width, unsigned height);

/**
 * Named feed preset matching W/H within ~1% relative error.
 * Returns kNone when the photo is not 3:4 / 4:5 / 1:1 / 1.91:1 (e.g. 3:2 stays Original).
 */
SliceAspect nearest_instagram_aspect(unsigned width, unsigned height);

/**
 * Even output size for a crop tile. requested 0 = native crop, clamped to Instagram 2×.
 * Never upscales. Floor is 1×, ceiling is min(crop, 2×).
 */
bool clamp_encode_output_size(SliceAspect aspect, unsigned crop_w, unsigned crop_h,
                              unsigned requested_w, unsigned requested_h, unsigned* width,
                              unsigned* height);

/**
 * Encode-time W×H for a master frame: Original uses the full even frame; named aspects use the
 * first cover-crop tile. requested 0 = smart 1× / native pick via clamp_encode_output_size.
 */
bool resolve_item_export_size(unsigned master_w, unsigned master_h, SliceAspect aspect,
                              unsigned requested_w, unsigned requested_h, unsigned* width,
                              unsigned* height);

/**
 * How many same-aspect tiles fit on master W×H, capped at kInstagramGalleryMax.
 * Returns 0 for kNone or when a tile cannot be formed.
 */
unsigned max_slice_count(unsigned master_w, unsigned master_h, SliceAspect aspect);

/**
 * Cover-crop tiles of the chosen aspect on an even-normalized master W×H.
 * Wide frames pack left-to-right at full height; tall frames pack top-to-bottom at full width.
 * crop_offset in [0, 1] slides leftover slack (0.5 = centered).
 * requested_count 0 = pack the maximum; otherwise clamp to [1, max].
 */
bool compute_slices(unsigned master_w, unsigned master_h, SliceAspect aspect,
                    std::vector<CropRect>* slices, std::string* error, float crop_offset = 0.5f,
                    unsigned requested_count = 1);

/** e.g. /path/photo.jpg + 4x5 + 1 → /path/photo_4x5_01.jpg */
std::string make_slice_output_path(const std::string& out_path, SliceAspect aspect, unsigned index);

}  // namespace uhdr_repack
