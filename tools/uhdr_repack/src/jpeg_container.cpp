#include "jpeg_container.h"

#include "path_io.h"

#include <iterator>

namespace uhdr_repack {

namespace {

bool jpeg_sof_size(const uint8_t* data, size_t len, unsigned* width, unsigned* height) {
  if (!data || len < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
  size_t i = 2;
  while (i + 9 < len) {
    if (data[i] != 0xff) {
      ++i;
      continue;
    }
    const uint8_t marker = data[i + 1];
    if (marker >= 0xc0 && marker <= 0xc3) {
      const unsigned h = (static_cast<unsigned>(data[i + 5]) << 8) | data[i + 6];
      const unsigned w = (static_cast<unsigned>(data[i + 7]) << 8) | data[i + 8];
      if (w < 1 || h < 1) return false;
      if (width) *width = w;
      if (height) *height = h;
      return true;
    }
    if (marker == 0xda) break;
    if (marker == 0xd8 || marker == 0xd9 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
      i += 2;
      continue;
    }
    if (i + 4 > len) break;
    const uint16_t seglen = static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
    if (seglen < 2) break;
    i += 2u + seglen;
  }
  return false;
}

}  // namespace

bool load_binary_file(const std::string& path, std::vector<uint8_t>* out, std::string* error) {
  if (!out) {
    if (error) *error = "null buffer";
    return false;
  }
  std::ifstream input = open_input_binary(path);
  if (!input) {
    if (error) *error = "Could not read: " + path;
    return false;
  }
  std::vector<char> raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (raw.empty()) {
    if (error) *error = "Empty file: " + path;
    return false;
  }
  out->assign(raw.begin(), raw.end());
  return true;
}

bool probe_jpeg_sof_size(const std::vector<uint8_t>& jpeg, unsigned* width, unsigned* height) {
  return jpeg_sof_size(jpeg.data(), jpeg.size(), width, height);
}

bool probe_jpeg_sof_size(const std::string& path, unsigned* width, unsigned* height,
                         std::string* error) {
  std::vector<uint8_t> jpeg;
  if (!load_binary_file(path, &jpeg, error)) return false;
  if (!probe_jpeg_sof_size(jpeg, width, height)) {
    if (error) *error = "Not a JPEG with SOF size: " + path;
    return false;
  }
  return true;
}

bool jpeg_first_scan_range(const std::vector<uint8_t>& jpeg, size_t* sos, size_t* eoi) {
  if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return false;
  size_t i = 2;
  size_t sos_off = 0;
  while (i + 1 < jpeg.size()) {
    if (jpeg[i] != 0xff) {
      ++i;
      continue;
    }
    const uint8_t marker = jpeg[i + 1];
    if (marker == 0xda) {
      sos_off = i;
      break;
    }
    if (marker == 0xd8 || marker == 0xd9 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
      i += 2;
      continue;
    }
    if (i + 4 > jpeg.size()) return false;
    const uint16_t seglen = static_cast<uint16_t>((jpeg[i + 2] << 8) | jpeg[i + 3]);
    if (seglen < 2) return false;
    i += 2u + seglen;
  }
  if (sos_off == 0) return false;
  for (size_t j = jpeg.size(); j >= 2; --j) {
    if (jpeg[j - 2] == 0xff && jpeg[j - 1] == 0xd9) {
      if (sos) *sos = sos_off;
      if (eoi) *eoi = j;
      return true;
    }
  }
  return false;
}

}  // namespace uhdr_repack
