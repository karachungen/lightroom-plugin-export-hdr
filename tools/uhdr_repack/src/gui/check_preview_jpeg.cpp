#include "gui/sdr_preview_image.h"

#include "path_io.h"

#include <QImage>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace uhdr_repack {
namespace fs = std::filesystem;

int check_preview_jpeg_main(const std::string& path) {
  QImage image;
  std::string error;
  if (!load_sdr_preview_image(path, &image, &error) || image.isNull()) {
    std::cerr << (error.empty() ? "Could not load SDR image" : error) << "\n";
    return 1;
  }
  std::vector<uint8_t> jpeg;
  if (!encode_preview_jpeg(image, 80, &jpeg, &error) || jpeg.size() < 32 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
    std::cerr << (error.empty() ? "Could not encode preview JPEG" : error) << "\n";
    return 1;
  }
  const fs::path out = fs::temp_directory_path() / "uhdr-preview-check.jpg";
  {
    std::ofstream file = open_output_binary(out.u8string());
    file.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    if (!file) {
      std::cerr << "could not write " << out.u8string() << "\n";
      return 1;
    }
  }
  QImage again;
  if (!load_sdr_preview_image(out.u8string(), &again, &error) || again.isNull()) {
    std::cerr << (error.empty() ? "encoded preview JPEG did not reload" : error) << "\n";
    return 1;
  }
  std::cout << "preview jpeg bytes: " << jpeg.size() << "\n";
  return 0;
}

}  // namespace uhdr_repack
