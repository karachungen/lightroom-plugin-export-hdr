#include "cli.h"

#include "encode_engine.h"
#include "gainmap_compute.h"
#include "slice_plan.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <string>

namespace uhdr_repack {

namespace {

bool parse_int(const char* s, int* out) {
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < 0 || v > 1000000) {
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

bool parse_float(const char* s, float* out) {
  char* end = nullptr;
  float v = std::strtof(s, &end);
  if (end == s || *end != '\0') {
    return false;
  }
  *out = v;
  return true;
}

}  // namespace

void print_usage() {
  std::cerr
      << "uhdr_repack — build Ultra HDR (.jpg with embedded HDR gain map) from Lightroom HDR TIFF + SDR base.\n\n"
      << "Usage:\n"
      << "  uhdr_repack --hdr-tiff <path> --base <path> --out <path.jpg> [options]\n"
      << "  uhdr_repack --inspect <path.jpg>\n"
      << "  uhdr_repack --edit [--session <json>] [--sdr <path> --hdr-tiff <path> --out <path>]...\n"
      << "  uhdr_repack --self-test [--session <json>]\n"
      << "  uhdr_repack --dump-gainmap --sdr <path> --hdr-tiff <path> --out <path.gainmap>\n\n"
      << "Encode options:\n"
      << "  --base-quality <0-100>       (default 95)\n"
      << "  --gainmap-quality <0-100>    (default 95)\n"
      << "  --gainmap-scale <N>          gain-map downsample factor vs base (1 = full size; default 1)\n"
      << "  --min-content-boost <linear>   (default 1.0)\n"
      << "  --max-content-boost <linear>   (default 16)\n"
      << "  --target-display-peak <nits>   (default 3250)\n"
      << "  --monochrome-gainmap           single-channel gain map\n"
      << "  --slice-aspect <none|1x1|4x5|3x4|191x100>  Instagram crop / carousel tiles\n"
      << "  --slice-count <N|max>        gallery slides (default 1; max = all that fit, cap 20)\n"
      << "  --crop-offset <0-1>          slide leftover crop slack (default 0.5 = centered)\n"
      << "  --out-width <px>             even output width (height follows aspect; 0 = native, cap 2x)\n"
      << "  --gainmap-in <path>            external gain map (float32 raw or edited buffer)\n"
      << "  --watermark-config <json>      vivid HDR text / watermark config\n"
      << "  --metadata-patch <json>        XMP/IPTC metadata patch\n";
}

int cli_encode_main(int argc, char** argv) {
  EncodeRequest req;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--hdr-tiff" && i + 1 < argc) {
      req.hdr_tiff = argv[++i];
    } else if (a == "--base" && i + 1 < argc) {
      req.base_path = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      req.out_path = argv[++i];
    } else if (a == "--slice-aspect" && i + 1 < argc) {
      if (!parse_slice_aspect(argv[++i], &req.slice_aspect)) {
        std::cerr << "bad --slice-aspect (use none, 1x1, 4x5, 3x4, or 191x100)\n";
        return 1;
      }
    } else if (a == "--slice-count" && i + 1 < argc) {
      const char* raw = argv[++i];
      if (std::strcmp(raw, "max") == 0 || std::strcmp(raw, "all") == 0) {
        req.slice_count = 0;
      } else {
        int n = 0;
        if (!parse_int(raw, &n) || n < 0) {
          std::cerr << "bad --slice-count (use a non-negative integer or max)\n";
          return 1;
        }
        req.slice_count = static_cast<unsigned>(n);
      }
    } else if (a == "--crop-offset" && i + 1 < argc) {
      float v = 0.f;
      if (!parse_float(argv[++i], &v) || v < 0.f || v > 1.f) {
        std::cerr << "bad --crop-offset (use 0..1)\n";
        return 1;
      }
      req.crop_offset = v;
    } else if (a == "--out-width" && i + 1 < argc) {
      int w = 0;
      if (!parse_int(argv[++i], &w)) {
        std::cerr << "bad --out-width\n";
        return 1;
      }
      req.output_width = static_cast<unsigned>(w);
    } else if (a == "--base-quality" && i + 1 < argc) {
      int q = 0;
      if (!parse_int(argv[++i], &q)) {
        std::cerr << "bad --base-quality\n";
        return 1;
      }
      req.options.base_quality = q;
    } else if (a == "--gainmap-quality" && i + 1 < argc) {
      int q = 0;
      if (!parse_int(argv[++i], &q)) {
        std::cerr << "bad --gainmap-quality\n";
        return 1;
      }
      req.options.gainmap_quality = q;
    } else if (a == "--gainmap-scale" && i + 1 < argc) {
      int s = 0;
      if (!parse_int(argv[++i], &s)) {
        std::cerr << "bad --gainmap-scale\n";
        return 1;
      }
      req.options.gainmap_scale = s;
    } else if (a == "--min-content-boost" && i + 1 < argc) {
      float v = 0.f;
      if (!parse_float(argv[++i], &v)) {
        std::cerr << "bad --min-content-boost\n";
        return 1;
      }
      req.options.min_content_boost = v;
    } else if (a == "--max-content-boost" && i + 1 < argc) {
      float v = 0.f;
      if (!parse_float(argv[++i], &v)) {
        std::cerr << "bad --max-content-boost\n";
        return 1;
      }
      req.options.max_content_boost = v;
    } else if (a == "--target-display-peak" && i + 1 < argc) {
      float v = 0.f;
      if (!parse_float(argv[++i], &v)) {
        std::cerr << "bad --target-display-peak\n";
        return 1;
      }
      req.options.target_display_peak_nits = v;
    } else if (a == "--monochrome-gainmap") {
      req.options.monochrome_gainmap = true;
    } else if (a == "--gainmap-in" && i + 1 < argc) {
      req.gainmap_in = argv[++i];
    } else if (a == "--watermark-config" && i + 1 < argc) {
      req.watermark_config = argv[++i];
    } else if (a == "--metadata-patch" && i + 1 < argc) {
      req.metadata_patch = argv[++i];
    } else {
      std::cerr << "unknown argument: " << a << "\n";
      print_usage();
      return 1;
    }
  }

  if (req.hdr_tiff.empty() || req.base_path.empty() || req.out_path.empty()) {
    print_usage();
    return 1;
  }

  std::string err;
  return encode_from_paths(req, &err);
}

int cli_dump_gainmap_main(int argc, char** argv) {
  std::string sdr;
  std::string hdr_tiff;
  std::string out;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--sdr" && i + 1 < argc) {
      sdr = argv[++i];
    } else if (a == "--hdr-tiff" && i + 1 < argc) {
      hdr_tiff = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out = argv[++i];
    }
  }

  if (sdr.empty() || hdr_tiff.empty() || out.empty()) {
    print_usage();
    return 1;
  }

  std::vector<float> gain;
  int w = 0;
  int h = 0;
  std::string err;
  if (!compute_auto_gainmap(sdr, hdr_tiff, &gain, &w, &h, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  if (!write_gainmap_raw_file(out, gain, w, h, &err)) {
    std::cerr << err << "\n";
    return 1;
  }

  float gmin = gain[0];
  float gmax = gain[0];
  for (float g : gain) {
    gmin = std::min(gmin, g);
    gmax = std::max(gmax, g);
  }
  std::cout << "Wrote gain map " << w << "x" << h << " range [" << gmin << ", " << gmax << "] -> "
            << out << "\n";
  return 0;
}

}  // namespace uhdr_repack
