#include "cli.h"

#include "path_io.h"
#include "sdr_jpeg.h"
#include "tiff_float.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace uhdr_repack {

int describe_input_main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "--describe-input requires an image path\n";
    return 1;
  }
  const std::string path = argv[2];
  bool require_portable = false;
  bool skip_if_missing = false;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--require-portable") == 0) require_portable = true;
    else if (std::strcmp(argv[i], "--skip-if-missing") == 0) skip_if_missing = true;
    else {
      std::cerr << "unknown --describe-input argument: " << argv[i] << "\n";
      return 1;
    }
  }
  if (!std::filesystem::exists(path_from_utf8(path))) {
    if (skip_if_missing) {
      std::cout << "SKIP: missing " << path << "\n";
      return 77;
    }
    std::cerr << "missing " << path << "\n";
    return 1;
  }
  std::string err;
  FloatTiffInfo tiff;
  if (!describe_float_tiff(path, &tiff, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  bool supported = false;
  std::string why;
  if (tiff.is_tiff) {
    std::cout << "TIFF " << tiff.width << "x" << tiff.height << " compression=" << tiff.compression
              << " predictor=" << tiff.predictor << (tiff.big_endian ? " MM" : " II")
              << " icc=" << (tiff.has_icc ? icc_class_name(tiff.icc) : std::string("none"));
    supported = tiff.supported;
    why = tiff.why;
  } else {
    SdrJpegInfo jpeg;
    if (!describe_sdr_jpeg(path, &jpeg, &err)) {
      std::cerr << err << "\n";
      return 1;
    }
    if (!jpeg.is_jpeg) {
      std::cerr << path << " is not a TIFF or JPEG\n";
      return 1;
    }
    const std::string icc_text = jpeg.has_icc              ? icc_class_name(jpeg.icc)
                                 : jpeg.supported          ? std::string("none (read as sRGB)")
                                                           : std::string("unreadable");
    std::cout << "JPEG " << jpeg.width << "x" << jpeg.height << " icc=" << icc_text;
    supported = jpeg.supported;
    why = jpeg.why;
  }
  std::cout << (supported ? std::string(" reader=portable") : " reader=os (" + why + ")") << "\n";
  return require_portable && !supported ? 1 : 0;
}

}  // namespace uhdr_repack
