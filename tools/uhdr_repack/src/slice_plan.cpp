#include "slice_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace uhdr_repack {

namespace {

unsigned even_floor(unsigned v) {
  if (v < 2) {
    return 0;
  }
  return v - (v % 2);
}

bool aspect_ratio(SliceAspect aspect, unsigned* rw, unsigned* rh) {
  if (!rw || !rh) {
    return false;
  }
  switch (aspect) {
    case SliceAspect::k1x1:
      *rw = 1;
      *rh = 1;
      return true;
    case SliceAspect::k4x5:
      *rw = 4;
      *rh = 5;
      return true;
    case SliceAspect::k3x4:
      *rw = 3;
      *rh = 4;
      return true;
    case SliceAspect::k191x100:
      *rw = 191;
      *rh = 100;
      return true;
    default:
      return false;
  }
}

unsigned even_offset(unsigned slack, float crop_offset) {
  const float t = std::clamp(crop_offset, 0.0f, 1.0f);
  unsigned start = static_cast<unsigned>(std::lround(static_cast<double>(slack) * t));
  start = even_floor(start);
  if (start > slack) {
    start = even_floor(slack);
  }
  return start;
}

struct TileGeometry {
  bool ok = false;
  bool pack_horizontal = true;
  unsigned tile_w = 0;
  unsigned tile_h = 0;
  unsigned max_count = 0;
  std::string error;
};

TileGeometry plan_tiles(unsigned master_w, unsigned master_h, SliceAspect aspect) {
  TileGeometry g;
  if (aspect == SliceAspect::kNone) {
    g.ok = true;
    return g;
  }
  unsigned w = even_floor(master_w);
  unsigned h = even_floor(master_h);
  if (w < 2 || h < 2) {
    return g;
  }
  master_w = w;
  master_h = h;

  unsigned rw = 0;
  unsigned rh = 0;
  if (!aspect_ratio(aspect, &rw, &rh)) {
    g.error = "Unknown Instagram aspect";
    return g;
  }

  const unsigned long long image_num = static_cast<unsigned long long>(master_w) * rh;
  const unsigned long long image_den = static_cast<unsigned long long>(master_h) * rw;
  g.pack_horizontal = image_num >= image_den;

  if (g.pack_horizontal) {
    g.tile_h = even_floor(master_h);
    g.tile_w = even_floor(static_cast<unsigned>((static_cast<unsigned long long>(g.tile_h) * rw) / rh));
    if (g.tile_w < 2 || g.tile_h < 2) {
      g.error = "Computed tile size is too small for slicing";
      return g;
    }
    g.max_count = master_w / g.tile_w;
    if (g.max_count < 1) {
      char buf[256];
      std::snprintf(buf, sizeof(buf), "Image %ux%u is too narrow for %s slices (tile %ux%u)",
                    master_w, master_h, slice_aspect_label(aspect), g.tile_w, g.tile_h);
      g.error = buf;
      return g;
    }
  } else {
    g.tile_w = even_floor(master_w);
    g.tile_h = even_floor(static_cast<unsigned>((static_cast<unsigned long long>(g.tile_w) * rh) / rw));
    if (g.tile_w < 2 || g.tile_h < 2) {
      g.error = "Computed tile size is too small for slicing";
      return g;
    }
    g.max_count = master_h / g.tile_h;
    if (g.max_count < 1) {
      char buf[256];
      std::snprintf(buf, sizeof(buf), "Image %ux%u is too short for %s slices (tile %ux%u)",
                    master_w, master_h, slice_aspect_label(aspect), g.tile_w, g.tile_h);
      g.error = buf;
      return g;
    }
  }
  if (g.max_count > kInstagramGalleryMax) {
    g.max_count = kInstagramGalleryMax;
  }
  g.ok = true;
  return g;
}

}  // namespace

bool parse_slice_aspect(const std::string& s, SliceAspect* out) {
  if (!out) {
    return false;
  }
  if (s.empty() || s == "none" || s == "off" || s == "original") {
    *out = SliceAspect::kNone;
    return true;
  }
  if (s == "1x1" || s == "1:1") {
    *out = SliceAspect::k1x1;
    return true;
  }
  if (s == "4x5" || s == "4:5") {
    *out = SliceAspect::k4x5;
    return true;
  }
  if (s == "3x4" || s == "3:4") {
    *out = SliceAspect::k3x4;
    return true;
  }
  if (s == "191x100" || s == "1.91:1" || s == "191:100" || s == "landscape") {
    *out = SliceAspect::k191x100;
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
    case SliceAspect::k3x4:
      return "3x4";
    case SliceAspect::k191x100:
      return "191x100";
    default:
      return "none";
  }
}

bool instagram_output_size(SliceAspect aspect, unsigned* width, unsigned* height) {
  if (!width || !height) {
    return false;
  }
  switch (aspect) {
    case SliceAspect::k1x1:
      *width = 1080;
      *height = 1080;
      return true;
    case SliceAspect::k4x5:
      *width = 1080;
      *height = 1350;
      return true;
    case SliceAspect::k3x4:
      *width = 1080;
      *height = 1440;
      return true;
    case SliceAspect::k191x100:
      *width = 1080;
      *height = 566;
      return true;
    default:
      *width = 0;
      *height = 0;
      return false;
  }
}

namespace {

constexpr double kIgMinRatio = 3.0 / 4.0;
constexpr double kIgMaxRatio = 191.0 / 100.0;
constexpr double kIgRangeEps = 0.01;
constexpr double kIgMatchRel = 0.01;

unsigned even_from_width(unsigned width, unsigned rw, unsigned rh) {
  if (rw == 0) {
    return 0;
  }
  return even_floor(static_cast<unsigned>((static_cast<unsigned long long>(width) * rh) / rw));
}

unsigned even_from_height(unsigned height, unsigned rw, unsigned rh) {
  if (rh == 0) {
    return 0;
  }
  return even_floor(static_cast<unsigned>((static_cast<unsigned long long>(height) * rw) / rh));
}

void size_from_width(SliceAspect aspect, unsigned crop_w, unsigned crop_h, unsigned width,
                     unsigned* out_w, unsigned* out_h) {
  unsigned w = even_floor(width);
  if (w < 2) {
    w = 2;
  }
  unsigned h = 0;
  unsigned rw = 0;
  unsigned rh = 0;
  if (aspect_ratio(aspect, &rw, &rh)) {
    h = even_from_width(w, rw, rh);
  } else if (crop_w >= 2) {
    h = even_floor(static_cast<unsigned>((static_cast<unsigned long long>(w) * crop_h) / crop_w));
  }
  if (h < 2) {
    h = 2;
  }
  *out_w = w;
  *out_h = h;
}

}  // namespace

bool instagram_output_size_scaled(SliceAspect aspect, unsigned scale, unsigned crop_w,
                                  unsigned crop_h, unsigned* width, unsigned* height) {
  if (!width || !height || (scale != 1 && scale != 2)) {
    return false;
  }
  unsigned w = 0;
  unsigned h = 0;
  if (instagram_output_size(aspect, &w, &h)) {
    w *= scale;
    h *= scale;
    w = even_floor(w);
    h = even_floor(h);
    if (w < 2 || h < 2) {
      return false;
    }
    *width = w;
    *height = h;
    return true;
  }
  crop_w = even_floor(crop_w);
  crop_h = even_floor(crop_h);
  if (crop_w < 2 || crop_h < 2) {
    return false;
  }
  size_from_width(aspect, crop_w, crop_h, 1080u * scale, &w, &h);
  if (w < 2 || h < 2) {
    return false;
  }
  *width = w;
  *height = h;
  return true;
}

bool instagram_aspect_supported(unsigned width, unsigned height) {
  if (width < 2 || height < 2) {
    return false;
  }
  const double r = static_cast<double>(width) / static_cast<double>(height);
  return r + kIgRangeEps >= kIgMinRatio && r - kIgRangeEps <= kIgMaxRatio;
}

SliceAspect nearest_instagram_aspect(unsigned width, unsigned height) {
  if (width < 2 || height < 2) {
    return SliceAspect::kNone;
  }
  const double actual = static_cast<double>(width) / static_cast<double>(height);
  const SliceAspect cands[] = {SliceAspect::k3x4, SliceAspect::k4x5, SliceAspect::k1x1,
                               SliceAspect::k191x100};
  for (SliceAspect aspect : cands) {
    unsigned rw = 0;
    unsigned rh = 0;
    if (!aspect_ratio(aspect, &rw, &rh) || rh == 0) {
      continue;
    }
    const double named = static_cast<double>(rw) / static_cast<double>(rh);
    if (std::abs(actual - named) / named <= kIgMatchRel) {
      return aspect;
    }
  }
  return SliceAspect::kNone;
}

bool clamp_encode_output_size(SliceAspect aspect, unsigned crop_w, unsigned crop_h,
                              unsigned requested_w, unsigned requested_h, unsigned* width,
                              unsigned* height) {
  if (!width || !height || crop_w < 2 || crop_h < 2) {
    return false;
  }
  crop_w = even_floor(crop_w);
  crop_h = even_floor(crop_h);
  if (crop_w < 2 || crop_h < 2) {
    return false;
  }

  unsigned x1_w = crop_w;
  unsigned x1_h = crop_h;
  unsigned x2_w = crop_w;
  unsigned x2_h = crop_h;
  if (!instagram_output_size_scaled(aspect, 1, crop_w, crop_h, &x1_w, &x1_h)) {
    x1_w = crop_w;
    x1_h = crop_h;
  }
  if (!instagram_output_size_scaled(aspect, 2, crop_w, crop_h, &x2_w, &x2_h)) {
    x2_w = crop_w;
    x2_h = crop_h;
  }

  unsigned min_w = x1_w;
  unsigned min_h = x1_h;
  if (min_w > crop_w || min_h > crop_h) {
    min_w = crop_w;
    min_h = crop_h;
  }
  unsigned max_w = x2_w;
  unsigned max_h = x2_h;
  if (max_w > crop_w || max_h > crop_h) {
    max_w = crop_w;
    max_h = crop_h;
  }

  unsigned w = crop_w;
  unsigned h = crop_h;
  if (requested_w >= 2) {
    size_from_width(aspect, crop_w, crop_h, requested_w, &w, &h);
  } else if (requested_h >= 2) {
    unsigned rw = 0;
    unsigned rh = 0;
    unsigned hh = even_floor(requested_h);
    if (aspect_ratio(aspect, &rw, &rh)) {
      w = even_from_height(hh, rw, rh);
      h = hh;
    } else if (crop_h >= 2) {
      w = even_floor(static_cast<unsigned>((static_cast<unsigned long long>(hh) * crop_w) / crop_h));
      h = hh;
    }
  }

  if (w < min_w) {
    size_from_width(aspect, crop_w, crop_h, min_w, &w, &h);
  }
  if (h < min_h) {
    unsigned rw = 0;
    unsigned rh = 0;
    if (aspect_ratio(aspect, &rw, &rh)) {
      w = even_from_height(min_h, rw, rh);
      h = min_h;
    } else {
      w = even_floor(static_cast<unsigned>((static_cast<unsigned long long>(min_h) * crop_w) / crop_h));
      h = min_h;
    }
  }
  if (w > max_w) {
    size_from_width(aspect, crop_w, crop_h, max_w, &w, &h);
  }
  if (h > max_h) {
    unsigned rw = 0;
    unsigned rh = 0;
    if (aspect_ratio(aspect, &rw, &rh)) {
      w = even_from_height(max_h, rw, rh);
      h = max_h;
    } else {
      w = max_w;
      h = max_h;
    }
  }
  if (w > crop_w) {
    w = crop_w;
  }
  if (h > crop_h) {
    h = crop_h;
  }
  if (w < 2 || h < 2) {
    return false;
  }
  *width = w;
  *height = h;
  return true;
}

bool resolve_item_export_size(unsigned master_w, unsigned master_h, SliceAspect aspect,
                              unsigned requested_w, unsigned requested_h, unsigned* width,
                              unsigned* height) {
  unsigned crop_w = master_w;
  unsigned crop_h = master_h;
  if (aspect != SliceAspect::kNone) {
    std::vector<CropRect> slices;
    std::string err;
    if (!compute_slices(master_w, master_h, aspect, &slices, &err, 0.5f, 1) || slices.empty()) {
      return false;
    }
    crop_w = slices[0].w;
    crop_h = slices[0].h;
  }
  return clamp_encode_output_size(aspect, crop_w, crop_h, requested_w, requested_h, width, height);
}

unsigned max_slice_count(unsigned master_w, unsigned master_h, SliceAspect aspect) {
  return plan_tiles(master_w, master_h, aspect).max_count;
}

bool compute_slices(unsigned master_w, unsigned master_h, SliceAspect aspect,
                    std::vector<CropRect>* slices, std::string* error, float crop_offset,
                    unsigned requested_count) {
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

  const TileGeometry g = plan_tiles(master_w, master_h, aspect);
  if (!g.ok) {
    if (error) {
      *error = g.error;
    }
    return false;
  }

  unsigned count = requested_count == 0 ? g.max_count : requested_count;
  if (count < 1) {
    count = 1;
  }
  if (count > g.max_count) {
    count = g.max_count;
  }

  unsigned slack = 0;
  unsigned start_x = 0;
  unsigned start_y = 0;
  if (g.pack_horizontal) {
    slack = master_w - count * g.tile_w;
    start_x = even_offset(slack, crop_offset);
    if (start_x + count * g.tile_w > master_w) {
      start_x = even_floor(master_w - count * g.tile_w);
    }
  } else {
    slack = master_h - count * g.tile_h;
    start_y = even_offset(slack, crop_offset);
    if (start_y + count * g.tile_h > master_h) {
      start_y = even_floor(master_h - count * g.tile_h);
    }
  }

  slices->reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    CropRect r;
    r.w = g.tile_w;
    r.h = g.tile_h;
    if (g.pack_horizontal) {
      r.x = start_x + i * g.tile_w;
      r.y = start_y;
    } else {
      r.x = start_x;
      r.y = start_y + i * g.tile_h;
    }
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
