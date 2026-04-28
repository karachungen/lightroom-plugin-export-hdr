#include "sdr_input.h"
#include "tiff_input.h"
#include "uhdr_encode.h"
#include "verify.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void PrintUsage() {
  std::cerr
      << "uhdr_repack — build Ultra HDR (.jpg with embedded HDR gain map) from Lightroom HDR TIFF + SDR base.\n\n"
      << "Usage:\n"
      << "  uhdr_repack --hdr-tiff <path> --base <path> --out <path.jpg> [options]\n"
      << "  uhdr_repack --inspect <path.jpg>\n\n"
      << "Options:\n"
      << "  --base-quality <0-100>       (default 92)\n"
      << "  --gainmap-quality <0-100>    (default 85)\n"
      << "  --gainmap-scale <N>          gain-map downsample factor vs base (1 = full size; default 1)\n"
      << "  --min-content-boost <linear>   (default 1.0)\n"
      << "  --max-content-boost <linear>   (default 1000)\n"
      << "  --target-display-peak <nits>   (default 1000)\n"
      << "  --monochrome-gainmap           single-channel gain map\n";
}

bool ParseInt(const char* s, int* out) {
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < 0 || v > 1000000) return false;
  *out = static_cast<int>(v);
  return true;
}

bool ParseFloat(const char* s, float* out) {
  char* end = nullptr;
  float v = std::strtof(s, &end);
  if (end == s || *end != '\0') return false;
  *out = v;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }

  if (std::strcmp(argv[1], "--inspect") == 0) {
    if (argc != 3) {
      PrintUsage();
      return 1;
    }
    uhdr_repack::InspectReport rep;
    std::string err;
    if (!uhdr_repack::inspect_ultra_hdr_file(argv[2], &rep, &err)) {
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

  std::string hdr_tiff;
  std::string base_path;
  std::string out_path;
  uhdr_repack::EncodeOptions opt;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--hdr-tiff" && i + 1 < argc) {
      hdr_tiff = argv[++i];
    } else if (a == "--base" && i + 1 < argc) {
      base_path = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (a == "--base-quality" && i + 1 < argc) {
      int q = 0;
      if (!ParseInt(argv[++i], &q)) {
        std::cerr << "bad --base-quality\n";
        return 1;
      }
      opt.base_quality = q;
    } else if (a == "--gainmap-quality" && i + 1 < argc) {
      int q = 0;
      if (!ParseInt(argv[++i], &q)) {
        std::cerr << "bad --gainmap-quality\n";
        return 1;
      }
      opt.gainmap_quality = q;
    } else if (a == "--gainmap-scale" && i + 1 < argc) {
      int s = 0;
      if (!ParseInt(argv[++i], &s)) {
        std::cerr << "bad --gainmap-scale\n";
        return 1;
      }
      opt.gainmap_scale = s;
    } else if (a == "--min-content-boost" && i + 1 < argc) {
      float v = 0.f;
      if (!ParseFloat(argv[++i], &v)) {
        std::cerr << "bad --min-content-boost\n";
        return 1;
      }
      opt.min_content_boost = v;
    } else if (a == "--max-content-boost" && i + 1 < argc) {
      float v = 0.f;
      if (!ParseFloat(argv[++i], &v)) {
        std::cerr << "bad --max-content-boost\n";
        return 1;
      }
      opt.max_content_boost = v;
    } else if (a == "--target-display-peak" && i + 1 < argc) {
      float v = 0.f;
      if (!ParseFloat(argv[++i], &v)) {
        std::cerr << "bad --target-display-peak\n";
        return 1;
      }
      opt.target_display_peak_nits = v;
    } else if (a == "--monochrome-gainmap") {
      opt.monochrome_gainmap = true;
    } else {
      std::cerr << "unknown argument: " << a << "\n";
      PrintUsage();
      return 1;
    }
  }

  if (hdr_tiff.empty() || base_path.empty() || out_path.empty()) {
    PrintUsage();
    return 1;
  }

  uhdr_repack::RawImageHolder hdr;
  uhdr_repack::RawImageHolder sdr;
  std::string err;

  if (!uhdr_repack::load_hdr_tiff_raw(hdr_tiff, &hdr, &err)) {
    std::cerr << "HDR TIFF: " << err << "\n";
    return 3;
  }

  unsigned w = hdr.ref().w;
  unsigned h = hdr.ref().h;

  if (!uhdr_repack::load_sdr_base_raw(base_path, w, h, &sdr, &err)) {
    std::cerr << "SDR base: " << err << "\n";
    return 4;
  }

  if (!uhdr_repack::encode_ultra_hdr_jpeg(hdr, sdr, opt, out_path, &err)) {
    std::cerr << "encode: " << err << "\n";
    return 5;
  }

  std::cout << "Wrote " << out_path << "\n";
  return 0;
}
