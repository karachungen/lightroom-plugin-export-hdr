#include "cli.h"

#include "encode_engine.h"
#include "encode_presets.h"
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
      << "  uhdr_repack --describe-input <tif-or-jpg> [--require-portable] [--skip-if-missing]\n"
      << "  uhdr_repack --edit [--session <json>] [--sdr <path> --hdr-tiff <path> --out <path>]...\n"
      << "  uhdr_repack --self-test [--session <json>]\n"
      << "  uhdr_repack --probe-sdr <path>\n"
      << "  uhdr_repack --encode-preview-jpeg <path>\n"
      << "  uhdr_repack --dump-gainmap --sdr <path> --hdr-tiff <path> --out <path.gainmap>\n"
      << "  uhdr_repack --write-hdr-chart <dir>\n"
      << "  uhdr_repack --check-hdr-chart <dir> [--out <jpg>] [--preset color|mono] [encode options]\n"
      << "  uhdr_repack --check-hdr-chart-file <jpg-or-tif> [--manifest <json>] [--slice-aspect <a>] [--crop-offset <0-1>]\n"
      << "  uhdr_repack --check-hdr-chart-assets <committed-dir> <fresh-dir>\n"
      << "  uhdr_repack --verify-uhdr <jpg> [--expect-size WxH] [--expect-single <aspect>] [--gainmap-scale N] [--max-bytes N] [--skip-if-missing]\n"
      << "  uhdr_repack --simulate-instagram <in.jpg> <out.jpg>   re-encode like Instagram (q61, 4:2:0)\n"
      << "  uhdr_repack --check-utf8-path <hdr-tiff> <sdr> <out-dir>\n"
      << "  uhdr_repack --encode-or-skip <encode arguments>\n"
      << "  uhdr_repack --check-preview-jpeg <sdr.jpg>\n\n"
      << "Encode options:\n"
      << "  --preset <color|mono>          editor delivery preset; later options override it\n"
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

int parse_encode_flag(int argc, char** argv, int* i, EncodeRequest* req) {
  const std::string a = argv[*i];
  if (*i + 1 >= argc) {
    if (a == "--monochrome-gainmap") {
      req->options.monochrome_gainmap = true;
      return 1;
    }
    return 0;
  }
  const char* v = argv[*i + 1];
  auto bad = [](const char* msg) {
    std::cerr << msg << "\n";
    return -1;
  };
  int n = 0;
  float f = 0.f;
  if (a == "--preset") {
    if (!apply_encode_preset(v, &req->options)) return bad("bad --preset (use color or mono)");
  } else if (a == "--slice-aspect") {
    if (!parse_slice_aspect(v, &req->slice_aspect)) return bad("bad --slice-aspect (use none, 1x1, 4x5, 3x4, or 191x100)");
  } else if (a == "--slice-count") {
    if (std::strcmp(v, "max") == 0 || std::strcmp(v, "all") == 0) req->slice_count = 0;
    else if (parse_int(v, &n)) req->slice_count = static_cast<unsigned>(n);
    else return bad("bad --slice-count (use a non-negative integer or max)");
  } else if (a == "--crop-offset") {
    if (!parse_float(v, &f) || f < 0.f || f > 1.f) return bad("bad --crop-offset (use 0..1)");
    req->crop_offset = f;
  } else if (a == "--out-width") {
    if (!parse_int(v, &n)) return bad("bad --out-width");
    req->output_width = static_cast<unsigned>(n);
  } else if (a == "--base-quality") {
    if (!parse_int(v, &n)) return bad("bad --base-quality");
    req->options.base_quality = n;
  } else if (a == "--gainmap-quality") {
    if (!parse_int(v, &n)) return bad("bad --gainmap-quality");
    req->options.gainmap_quality = n;
  } else if (a == "--gainmap-scale") {
    if (!parse_int(v, &n)) return bad("bad --gainmap-scale");
    req->options.gainmap_scale = n;
  } else if (a == "--min-content-boost") {
    if (!parse_float(v, &f)) return bad("bad --min-content-boost");
    req->options.min_content_boost = f;
  } else if (a == "--max-content-boost") {
    if (!parse_float(v, &f)) return bad("bad --max-content-boost");
    req->options.max_content_boost = f;
  } else if (a == "--target-display-peak") {
    if (!parse_float(v, &f)) return bad("bad --target-display-peak");
    req->options.target_display_peak_nits = f;
  } else if (a == "--gainmap-in") {
    req->gainmap_in = v;
  } else if (a == "--watermark-config") {
    req->watermark_config = v;
  } else if (a == "--metadata-patch") {
    req->metadata_patch = v;
  } else if (a == "--monochrome-gainmap") {
    req->options.monochrome_gainmap = true;
    return 1;
  } else {
    return 0;
  }
  ++*i;
  return 1;
}

int cli_encode_main(int argc, char** argv) {
  EncodeRequest req;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--hdr-tiff" && i + 1 < argc) {
      req.hdr_tiff = argv[++i];
    } else if (a == "--base" && i + 1 < argc) {
      req.base_path = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      req.out_path = argv[++i];
    } else {
      const int used = parse_encode_flag(argc, argv, &i, &req);
      if (used < 0) return 1;
      if (used == 0) {
        std::cerr << "unknown argument: " << a << "\n";
        print_usage();
        return 1;
      }
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
