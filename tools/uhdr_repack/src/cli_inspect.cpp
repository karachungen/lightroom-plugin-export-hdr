#include "cli.h"

#include "verify.h"

#include <cstring>
#include <iostream>
#include <string>

namespace uhdr_repack {

int cli_inspect_main(int argc, char** argv) {
  if (argc != 3 || std::strcmp(argv[1], "--inspect") != 0) {
    print_usage();
    return 1;
  }

  InspectReport rep;
  std::string err;
  if (!inspect_ultra_hdr_file(argv[2], &rep, &err)) {
    std::cerr << "inspect failed: " << err << "\n";
    return 2;
  }

  std::cout << "file: " << argv[2] << "\n";
  std::cout << "is_ultra_hdr: " << (rep.is_ultra_hdr ? "yes" : "no") << "\n";
  std::cout << "dimensions: " << rep.width << "x" << rep.height << "\n";
  std::cout << "gainmap_size: " << rep.gainmap_width << "x" << rep.gainmap_height << "\n";
  std::cout << "primary_jpeg_420: " << (rep.primary_jpeg_420 ? "likely" : "no") << "\n";
  std::cout << "gainmap_jpeg_420: " << (rep.gainmap_jpeg_420 ? "likely" : "no") << "\n";
  std::cout << "markers: MPF=" << rep.has_mpf << " primary_xmp=" << rep.has_primary_xmp
            << " iso_app2_hint=" << rep.has_iso_app2 << "\n";
  std::cout << "detail: " << rep.detail << "\n";
  return 0;
}

}  // namespace uhdr_repack
