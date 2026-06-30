#include "sdr_input.h"
#include "slice_plan.h"
#include "tiff_input.h"
#include "uhdr_encode.h"
#include "verify.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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
      << "  --monochrome-gainmap           single-channel gain map\n"
      << "  --slice-aspect <none|1x1|4x5>  optional full-height slices (numbered files next to --out)\n";
}

bool ParseInt(const char* s, int* out) {
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < 0 || v > 1000000) {
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

bool ParseFloat(const char* s, float* out) {
  char* end = nullptr;
  float v = std::strtof(s, &end);
  if (end == s || *end != '\0') {
    return false;
  }
  *out = v;
  return true;
}

bool EncodePair(const uhdr_repack::RawImageHolder& hdr, const uhdr_repack::RawImageHolder& sdr,
                const uhdr_repack::EncodeOptions& opt, const std::string& out_path,
                std::string* err) {
  if (!uhdr_repack::encode_ultra_hdr_jpeg(hdr, sdr, opt, out_path, err)) {
    std::cerr << "encode: " << *err << "\n";
    return false;
  }
  std::cout << "Wrote " << out_path << "\n";
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
  uhdr_repack::SliceAspect slice_aspect = uhdr_repack::SliceAspect::kNone;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--hdr-tiff" && i + 1 < argc) {
      hdr_tiff = argv[++i];
    } else if (a == "--base" && i + 1 < argc) {
      base_path = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (a == "--slice-aspect" && i + 1 < argc) {
      if (!uhdr_repack::parse_slice_aspect(argv[++i], &slice_aspect)) {
        std::cerr << "bad --slice-aspect (use none, 1x1, or 4x5)\n";
        return 1;
      }
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

  const unsigned master_w = hdr.ref().w;
  const unsigned master_h = hdr.ref().h;

  if (!uhdr_repack::load_sdr_base_raw(base_path, master_w, master_h, &sdr, &err)) {
    std::cerr << "SDR base: " << err << "\n";
    return 4;
  }

  if (!EncodePair(hdr, sdr, opt, out_path, &err)) {
    return 5;
  }

  if (slice_aspect == uhdr_repack::SliceAspect::kNone) {
    return 0;
  }

  std::vector<uhdr_repack::CropRect> slices;
  if (!uhdr_repack::compute_slices(master_w, master_h, slice_aspect, &slices, &err)) {
    std::cerr << "slice plan: " << err << "\n";
    return 6;
  }

  std::cerr << "Slicing " << master_w << "x" << master_h << " into " << slices.size() << " "
            << uhdr_repack::slice_aspect_label(slice_aspect) << " tile(s)\n";

  unsigned idx = 1;
  for (const auto& crop : slices) {
    uhdr_repack::RawImageHolder hdr_slice;
    uhdr_repack::RawImageHolder sdr_slice;

    if (!uhdr_repack::load_hdr_tiff_raw(hdr_tiff, &hdr_slice, &err, master_w, master_h, &crop)) {
      std::cerr << "HDR slice " << idx << ": " << err << "\n";
      return 7;
    }
    if (!uhdr_repack::load_sdr_base_raw(base_path, master_w, master_h, &sdr_slice, &err, &crop)) {
      std::cerr << "SDR slice " << idx << ": " << err << "\n";
      return 8;
    }

    const std::string slice_out = uhdr_repack::make_slice_output_path(out_path, slice_aspect, idx);
    std::cerr << "Slice " << idx << ": crop " << crop.x << "," << crop.y << " " << crop.w << "x"
              << crop.h << " -> " << slice_out << "\n";

    if (!EncodePair(hdr_slice, sdr_slice, opt, slice_out, &err)) {
      return 9;
    }
    ++idx;
  }

  return 0;
}
