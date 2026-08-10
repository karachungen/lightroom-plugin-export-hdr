#pragma once

#include <string>

namespace uhdr_repack {

struct InspectReport {
  bool is_ultra_hdr = false;
  int width = -1;
  int height = -1;
  int gainmap_width = -1;
  int gainmap_height = -1;
  int gainmap_components = -1;
  bool primary_jpeg_420 = false;
  bool gainmap_jpeg_420 = false;
  bool has_mpf = false;
  bool has_primary_xmp = false;
  bool has_iso_app2 = false;
  float gainmap_min_log2 = 0.0f;
  float gainmap_max_log2 = 0.0f;
  float gainmap_gamma = 0.0f;
  float gainmap_offset_sdr = 0.0f;
  float gainmap_offset_hdr = 0.0f;
  float hdr_capacity_min = 0.0f;
  float hdr_capacity_max = 0.0f;
  std::string detail;
};

/** Inspect output JPG (Ultra HDR container + JPEG structure). */
bool inspect_ultra_hdr_file(const std::string& path, InspectReport* report, std::string* error);

}  // namespace uhdr_repack
