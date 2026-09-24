#include "gui/sdr_preview_image.h"

#include "path_io.h"

#include <QImage>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace uhdr_repack {
namespace fs = std::filesystem;

namespace {

fs::path executable_directory() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  buffer.resize(length);
  const fs::path parent = fs::path(buffer).parent_path();
  return parent.empty() ? fs::path() : parent;
#elif defined(__APPLE__)
  char small[4096];
  uint32_t size = sizeof(small);
  if (_NSGetExecutablePath(small, &size) == 0) {
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(fs::path(small), ec);
    const fs::path parent = (ec ? fs::path(small) : resolved).parent_path();
    return parent.empty() ? fs::path() : parent;
  }
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  std::error_code ec;
  const fs::path resolved = fs::weakly_canonical(fs::path(buffer.c_str()), ec);
  const fs::path parent = (ec ? fs::path(buffer.c_str()) : resolved).parent_path();
  return parent.empty() ? fs::path() : parent;
#else
  std::error_code ec;
  const fs::path resolved = fs::read_symlink("/proc/self/exe", ec);
  if (ec) {
    return {};
  }
  const fs::path parent = resolved.parent_path();
  return parent.empty() ? fs::path() : parent;
#endif
}

unsigned long current_process_id() {
#ifdef _WIN32
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

fs::path preview_check_directory() {
  const fs::path exe_dir = executable_directory();
  if (!exe_dir.empty()) {
    return exe_dir / "test-out";
  }
  const fs::path build_dir = fs::current_path() / "tools" / "uhdr_repack" / "build";
  std::error_code ec;
  if (fs::exists(build_dir / "uhdr_repack.exe", ec) && !ec) {
    return build_dir / "test-out";
  }
  return {};
}

class RemoveFile {
 public:
  explicit RemoveFile(fs::path path) : path_(std::move(path)) {}
  ~RemoveFile() {
    std::error_code ec;
    fs::remove(path_, ec);
  }
  RemoveFile(const RemoveFile&) = delete;
  RemoveFile& operator=(const RemoveFile&) = delete;

 private:
  fs::path path_;
};

}  // namespace

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
  const fs::path dir = preview_check_directory();
  if (dir.empty()) {
    std::cerr << "could not write preview jpeg\n";
    return 1;
  }
  const fs::path out = dir / ("uhdr-preview-check-" + std::to_string(current_process_id()) + ".jpg");
  const RemoveFile cleanup(out);
  std::error_code ec;
  fs::create_directories(dir, ec);
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
