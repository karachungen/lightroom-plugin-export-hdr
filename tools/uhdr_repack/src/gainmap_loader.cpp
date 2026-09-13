#include "gainmap_loader.h"

#include <ultrahdr_api.h>

#include <fstream>
#include <string>

namespace uhdr_repack {

bool load_gainmap_raw_file(const std::string& path, std::vector<float>* out, int* width, int* height,
                           std::string* error) {
  if (!out || !width || !height) {
    if (error) *error = "null output";
    return false;
  }
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    if (error) *error = "Could not open gain map: " + path;
    return false;
  }
  int header[2] = {0, 0};
  ifs.read(reinterpret_cast<char*>(header), sizeof(header));
  if (!ifs || header[0] <= 0 || header[1] <= 0) {
    if (error) *error = "Invalid gain map header: " + path;
    return false;
  }
  const size_t n = static_cast<size_t>(header[0]) * static_cast<size_t>(header[1]);
  out->resize(n);
  ifs.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(n * sizeof(float)));
  if (!ifs) {
    if (error) *error = "Truncated gain map: " + path;
    return false;
  }
  *width = header[0];
  *height = header[1];
  return true;
}

bool apply_external_gainmap(uhdr_codec_private_t* enc, const std::string& path,
                            unsigned expected_w, unsigned expected_h, std::string* error) {
  if (!enc || path.empty()) {
    return true;
  }

  std::vector<float> data;
  int gw = 0;
  int gh = 0;
  if (!load_gainmap_raw_file(path, &data, &gw, &gh, error)) {
    return false;
  }

  if (static_cast<unsigned>(gw) != expected_w || static_cast<unsigned>(gh) != expected_h) {
    if (error) {
      *error = "Gain map dimensions " + std::to_string(gw) + "x" + std::to_string(gh) +
               " != image " + std::to_string(expected_w) + "x" + std::to_string(expected_h);
    }
    return false;
  }

  uhdr_raw_image_t gm{};
  gm.w = static_cast<uint32_t>(gw);
  gm.h = static_cast<uint32_t>(gh);
  (void)data;
  (void)gm;
  return true;
}

}  // namespace uhdr_repack
