#pragma once

#include "slice_plan.h"
#include "uhdr_encode.h"

#include <string>

namespace uhdr_repack {

struct EncodeRequest {
  std::string hdr_tiff;
  std::string base_path;
  std::string out_path;
  EncodeOptions options;
  std::string gainmap_in;
  std::string watermark_config;
  std::string metadata_patch;
  SliceAspect slice_aspect = SliceAspect::kNone;
  float crop_offset = 0.5f;
  /** 0 = pack maximum tiles; otherwise clamp to [1, max]. Default is one slide. */
  unsigned slice_count = 1;
  /** 0 = all tiles (Apply All). 1-based index encodes one tile to out_path (HDR preview). */
  unsigned preview_slice_index = 0;
  /** 0 = native resolution. Preview encodes cap the long edge (even). */
  unsigned preview_max_edge = 0;
  /** 0 = native crop, clamped to min(crop, 2×). Otherwise even pixels, aspect-locked, between 1× and min(crop, 2×). */
  unsigned output_width = 0;
  unsigned output_height = 0;
};

/** Load HDR+SDR and encode one Ultra HDR JPEG (optional slices). */
int encode_from_paths(const EncodeRequest& req, std::string* error_out);

/** Build argv-style encode from request (for logging). */
std::string format_encode_command_line(const EncodeRequest& req);

}  // namespace uhdr_repack
