#pragma once

#include "raw_image.h"
#include "slice_plan.h"

#include <string>
#include <vector>

namespace uhdr_repack {

/** Per-pixel scalar gain (HDR luma / SDR luma) from an SDR base + HDR TIFF pair. */
bool compute_auto_gainmap(const std::string& sdr_path, const std::string& hdr_tiff_path,
                          std::vector<float>* gain, int* width, int* height, std::string* error);

/**
 * Bake an edited luma gain map into the HDR buffer in Rec.2020:
 * HDR = linear_sRGB_to_Rec2020(SDR) * gain. Optional crop indexes a full-size map.
 */
bool apply_gainmap_to_hdr(RawImageHolder* hdr, const std::string& sdr_path,
                          const std::string& gainmap_path, std::string* error,
                          unsigned master_width = 0, unsigned master_height = 0,
                          const CropRect* crop = nullptr);

/** Write .gainmap file (int32 w,h header + float32 samples). */
bool write_gainmap_raw_file(const std::string& path, const std::vector<float>& gain, int width,
                            int height, std::string* error);

}  // namespace uhdr_repack
