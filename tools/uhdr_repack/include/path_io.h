#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace uhdr_repack {

inline std::filesystem::path path_from_utf8(const std::string& path) {
  return std::filesystem::u8path(path);
}

inline std::ofstream open_output_binary(const std::string& path) {
  return std::ofstream(path_from_utf8(path), std::ios::binary);
}

inline std::ifstream open_input_binary(const std::string& path) {
  return std::ifstream(path_from_utf8(path), std::ios::binary);
}

}  // namespace uhdr_repack
