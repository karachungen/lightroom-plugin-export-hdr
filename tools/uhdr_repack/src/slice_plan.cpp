#include "slice_plan.h"

#include <cstdio>

namespace uhdr_repack {

namespace {

unsigned even_floor(unsigned v) {
  if (v < 2) {
    return 0;
  }
  return v - (v % 2);
}

unsigned tile_width_for_aspect(unsigned master_h, SliceAspect aspect) {
  if (aspect == SliceAspect::k1x1) {
    return even_floor(master_h);
  }
  if (aspect == SliceAspect::k4x5) {
    return even_floor(static_cast<unsigned>((static_cast<unsigned long long>(master_h) * 4) / 5));
  }
  return 0;
}

}  // namespace

bool parse_slice_aspect(const std::string& s, SliceAspect* out) {
  if (!out) {
    return false;
  }
  if (s.empty() || s == "none" || s == "off") {
    *out = SliceAspect::kNone;
    return true;
  }
  if (s == "1x1") {
    *out = SliceAspect::k1x1;
    return true;
  }
  if (s == "4x5") {
    *out = SliceAspect::k4x5;
    return true;
  }
  return false;
}

const char* slice_aspect_label(SliceAspect aspect) {
  switch (aspect) {
    case SliceAspect::k1x1:
      return "1x1";
    case SliceAspect::k4x5:
      return "4x5";
    default:
      return "none";
  }
}

bool compute_slices(unsigned master_w, unsigned master_h, SliceAspect aspect,
                    std::vector<CropRect>* slices, std::string* error) {
  if (!slices) {
    if (error) {
      *error = "internal: null slices vector";
    }
    return false;
  }
  slices->clear();

  if (aspect == SliceAspect::kNone) {
    return true;
  }

  if (master_w < 2 || master_h < 2 || (master_w % 2) || (master_h % 2)) {
    if (error) {
      *error = "Master dimensions must be even and at least 2x2 for slicing";
    }
    return false;
  }

  const unsigned tile_w = tile_width_for_aspect(master_h, aspect);
  if (tile_w < 2) {
    if (error) {
      *error = "Computed tile width is too small for slicing";
    }
    return false;
  }

  const unsigned count = master_w / tile_w;
  if (count < 1) {
    if (error) {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "Image %ux%u is too narrow for %s slices (tile width %u at full height)",
                    master_w, master_h, slice_aspect_label(aspect), tile_w);
      *error = buf;
    }
    return false;
  }

  const unsigned covered_w = count * tile_w;
  const unsigned slack = master_w - covered_w;
  unsigned start_x = even_floor(slack / 2);

  if (start_x + covered_w > master_w) {
    start_x = even_floor(master_w - covered_w);
  }

  slices->reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    CropRect r;
    r.x = start_x + i * tile_w;
    r.y = 0;
    r.w = tile_w;
    r.h = master_h;
    slices->push_back(r);
  }

  return true;
}

std::string make_slice_output_path(const std::string& out_path, SliceAspect aspect,
                                   unsigned index) {
  const std::string label = slice_aspect_label(aspect);
  const size_t slash = out_path.find_last_of("/\\");
  const size_t dot = out_path.find_last_of('.');
  const bool has_ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);

  std::string folder;
  std::string leaf;
  if (slash != std::string::npos) {
    folder = out_path.substr(0, slash + 1);
    leaf = out_path.substr(slash + 1);
  } else {
    leaf = out_path;
  }

  std::string base = leaf;
  std::string ext;
  if (has_ext) {
    const size_t leaf_dot = leaf.find_last_of('.');
    base = leaf.substr(0, leaf_dot);
    ext = leaf.substr(leaf_dot);
  } else {
    ext = ".jpg";
  }

  char idx[8];
  std::snprintf(idx, sizeof(idx), "%02u", index);

  return folder + base + "_" + label + "_" + idx + ext;
}

}  // namespace uhdr_repack
