#include "delivery_check.h"

#include "cli.h"
#include "encode_engine.h"
#include "icc_profile.h"
#include "jpeg_container.h"
#include "path_io.h"
#include "session.h"
#include "slice_plan.h"
#include "verify.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace uhdr_repack {
namespace fs = std::filesystem;

namespace {

bool contains(const std::vector<uint8_t>& bytes, const char* needle) {
  const std::string hay(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return hay.find(needle) != std::string::npos;
}

bool parse_size(const std::string& spec, int* w, int* h) {
  const size_t x = spec.find('x');
  if (x == std::string::npos) return false;
  *w = std::atoi(spec.c_str());
  *h = std::atoi(spec.c_str() + x + 1);
  return *w >= 2 && *h >= 2 && std::to_string(*w) + "x" + std::to_string(*h) == spec;
}

bool scaled_ok(int image, int map, int scale) {
  return map == image / scale || map == (image + scale - 1) / scale;
}

}  // namespace

int verify_uhdr_file(const std::string& path, const UhdrExpect& expect) {
  if (!fs::exists(path_from_utf8(path))) {
    if (expect.skip_if_missing) {
      std::cout << "SKIP: missing " << path << "\n";
      return 77;
    }
    std::cerr << "missing " << path << "\n";
    return 1;
  }
  std::vector<uint8_t> bytes;
  std::string err;
  if (!load_binary_file(path, &bytes, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  InspectReport report;
  if (!inspect_ultra_hdr_file(path, &report, &err) || !report.is_ultra_hdr) {
    std::cerr << (err.empty() ? "not Ultra HDR: " + path : err) << "\n";
    return 1;
  }
  std::vector<std::string> problems;
  if (!report.has_mpf) problems.push_back("MPF index is missing");
  if (!report.has_primary_xmp) problems.push_back("primary hdrgm XMP is missing");
  if (!report.has_iso_app2) problems.push_back("ISO 21496-1 APP2 block is missing");
  const int s = expect.gainmap_scale < 1 ? 1 : expect.gainmap_scale;
  if (!scaled_ok(report.width, report.gainmap_width, s) || !scaled_ok(report.height, report.gainmap_height, s)) {
    problems.push_back("gain map is " + std::to_string(report.gainmap_width) + "x" +
                       std::to_string(report.gainmap_height) + ", expected image / " + std::to_string(s));
  }
  std::vector<uint8_t> icc;
  if (!read_embedded_icc(path, &icc, &err)) {
    problems.push_back("primary ICC: " + err);
  } else {
    const IccClass c = classify_icc(icc);
    if (c.primaries != IccPrimaries::kDisplayP3 || c.transfer != IccTransfer::kSrgb) {
      problems.push_back("primary ICC is " + icc_class_name(c) + "; browsers need Display P3 / sRGB curve");
    }
  }
  if (!contains(bytes, "https://hdr.karachun.by/") ||
      !contains(bytes, "https://github.com/karachungen/lightroom-plugin-export-hdr")) {
    problems.push_back("xmpRights URLs are missing");
  }
  if (expect.width > 0 && (report.width != expect.width || report.height != expect.height)) {
    problems.push_back("size " + std::to_string(report.width) + "x" + std::to_string(report.height) + " != " +
                       std::to_string(expect.width) + "x" + std::to_string(expect.height));
  }
  if (expect.max_bytes > 0 && bytes.size() > expect.max_bytes) {
    problems.push_back(std::to_string(bytes.size()) + " bytes is over the " + std::to_string(expect.max_bytes) +
                       "-byte limit");
  }
  if (!expect.single_aspect.empty()) {
    SliceAspect aspect = SliceAspect::kNone;
    if (!parse_slice_aspect(expect.single_aspect, &aspect)) {
      problems.push_back("unknown aspect " + expect.single_aspect);
    } else if (!list_slice_output_paths(path, aspect).empty()) {
      problems.push_back("numbered slices were written next to " + path);
    }
  }
  if (!problems.empty()) {
    for (const std::string& p : problems) std::cerr << p << "\n";
    std::cout << "UHDR_VERIFY FAIL " << path << "\n";
    return 1;
  }
  std::cout << "UHDR_VERIFY PASS " << report.width << "x" << report.height << " gain map " << report.gainmap_width
            << "x" << report.gainmap_height << " " << bytes.size() << " bytes\n";
  return 0;
}

int verify_uhdr_main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "--verify-uhdr requires a JPEG path\n";
    return 1;
  }
  UhdrExpect expect;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_value = i + 1 < argc;
    if (a == "--expect-size" && has_value) {
      if (!parse_size(argv[++i], &expect.width, &expect.height)) {
        std::cerr << "--expect-size wants WxH\n";
        return 1;
      }
    } else if (a == "--expect-single" && has_value) {
      expect.single_aspect = argv[++i];
    } else if (a == "--gainmap-scale" && has_value) {
      expect.gainmap_scale = std::atoi(argv[++i]);
    } else if (a == "--max-bytes" && has_value) {
      expect.max_bytes = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (a == "--skip-if-missing") {
      expect.skip_if_missing = true;
    } else {
      std::cerr << "unknown --verify-uhdr argument: " << a << "\n";
      return 1;
    }
  }
  return verify_uhdr_file(argv[2], expect);
}

int check_utf8_path_main(const std::string& hdr, const std::string& sdr, const std::string& out_dir) {
  const char kFolder[] = "\xd1\x82\xd0\xb5\xd1\x81\xd1\x82";  // тест, as UTF-8 bytes whatever the compiler code page
  const fs::path dir = path_from_utf8(out_dir) / path_from_utf8(kFolder);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    std::cerr << "could not create " << dir.u8string() << ": " << ec.message() << "\n";
    return 1;
  }
  EncodeRequest req;
  req.hdr_tiff = hdr;
  req.base_path = sdr;
  req.out_path = (dir / "out_uhdr.jpg").u8string();
  std::string err;
  const int enc = encode_from_paths(req, &err);
  if (enc != 0) {
    std::cerr << err << "\n";
    return enc;
  }
  return verify_uhdr_file(req.out_path, UhdrExpect{});
}

int encode_or_skip_main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) {
    if (i != 1) args.emplace_back(argv[i]);
  }
  for (size_t i = 1; i + 1 < args.size(); ++i) {
    if ((args[i] == "--hdr-tiff" || args[i] == "--base") && !fs::exists(path_from_utf8(args[i + 1]))) {
      std::cout << "SKIP: missing " << args[i + 1] << "\n";
      return 77;
    }
  }
  std::vector<char*> ptrs;
  for (std::string& a : args) ptrs.push_back(a.data());
  ptrs.push_back(nullptr);
  return cli_encode_main(static_cast<int>(args.size()), ptrs.data());
}

}  // namespace uhdr_repack
