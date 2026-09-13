#include "encode_engine.h"

#include "gainmap_compute.h"
#include "resize_lanczos.h"
#include "sdr_input.h"
#include "slice_plan.h"
#include "tiff_input.h"
#include "uhdr_encode.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace uhdr_repack {

namespace fs = std::filesystem;

namespace {

bool encode_pair(const RawImageHolder& hdr, const RawImageHolder& sdr, const EncodeOptions& opt,
                 const std::string& out_path, std::string* err) {
  if (!encode_ultra_hdr_jpeg(hdr, sdr, opt, out_path, err)) {
    std::cerr << "encode: " << *err << "\n";
    return false;
  }
  std::cout << "Wrote " << out_path << "\n";
  return true;
}

void fit_even_max_edge(unsigned src_w, unsigned src_h, unsigned max_edge, unsigned* dw,
                       unsigned* dh) {
  unsigned w = src_w;
  unsigned h = src_h;
  if (max_edge >= 2 && (w > max_edge || h > max_edge)) {
    const double scale = static_cast<double>(max_edge) / static_cast<double>(std::max(w, h));
    w = std::max(2u, static_cast<unsigned>(std::lround(w * scale)));
    h = std::max(2u, static_cast<unsigned>(std::lround(h * scale)));
  }
  if (w % 2) --w;
  if (h % 2) --h;
  if (w < 2) w = 2;
  if (h < 2) h = 2;
  *dw = w;
  *dh = h;
}

bool encode_target_size(const EncodeRequest& req, unsigned src_w, unsigned src_h, unsigned* dw,
                        unsigned* dh) {
  unsigned w = src_w;
  unsigned h = src_h;
  if (!clamp_encode_output_size(req.slice_aspect, src_w, src_h, req.output_width, req.output_height,
                                &w, &h)) {
    w = src_w;
    h = src_h;
  }
  if (req.preview_max_edge >= 2) {
    fit_even_max_edge(w, h, req.preview_max_edge, &w, &h);
  }
  *dw = w;
  *dh = h;
  return w != src_w || h != src_h;
}

bool encode_crop(const EncodeRequest& req, unsigned master_w, unsigned master_h,
                 const CropRect& crop, const EncodeOptions& opt, const std::string& out_path,
                 std::string* err) {
  unsigned dst_w = 0;
  unsigned dst_h = 0;
  const bool scale_in_load =
      req.preview_max_edge >= 2 && req.gainmap_in.empty() &&
      encode_target_size(req, crop.w, crop.h, &dst_w, &dst_h);

  RawImageHolder hdr_slice;
  RawImageHolder sdr_slice;
  if (!load_hdr_tiff_raw(req.hdr_tiff, &hdr_slice, err, master_w, master_h, &crop,
                         scale_in_load ? dst_w : 0, scale_in_load ? dst_h : 0)) {
    return false;
  }
  if (!load_sdr_base_raw(req.base_path, master_w, master_h, &sdr_slice, err, &crop)) {
    return false;
  }
  if (!req.gainmap_in.empty() &&
      !apply_gainmap_to_hdr(&hdr_slice, req.base_path, req.gainmap_in, err, master_w, master_h,
                            &crop)) {
    return false;
  }

  unsigned out_w = 0;
  unsigned out_h = 0;
  RawImageHolder hdr_out;
  RawImageHolder sdr_out;
  const RawImageHolder* hdr_enc = &hdr_slice;
  const RawImageHolder* sdr_enc = &sdr_slice;
  if (encode_target_size(req, hdr_slice.ref().w, hdr_slice.ref().h, &out_w, &out_h)) {
    if (hdr_slice.ref().w != out_w || hdr_slice.ref().h != out_h) {
      if (!resize_hdr_lanczos(hdr_slice, out_w, out_h, &hdr_out, err)) {
        return false;
      }
      hdr_enc = &hdr_out;
    }
    if (sdr_slice.ref().w != out_w || sdr_slice.ref().h != out_h) {
      if (!resize_sdr_lanczos_sharpen(sdr_slice, out_w, out_h, &sdr_out, err)) {
        return false;
      }
      sdr_enc = &sdr_out;
    }
  }
  return encode_pair(*hdr_enc, *sdr_enc, opt, out_path, err);
}

}  // namespace

std::string format_encode_command_line(const EncodeRequest& req) {
  std::ostringstream os;
  os << "uhdr_repack --hdr-tiff " << req.hdr_tiff << " --base " << req.base_path << " --out "
     << req.out_path;
  return os.str();
}

int encode_from_paths(const EncodeRequest& req, std::string* error_out) {
  std::string err;
  RawImageHolder hdr;
  RawImageHolder sdr;

  unsigned master_w = 0;
  unsigned master_h = 0;
  if (req.preview_max_edge >= 2 && req.slice_aspect == SliceAspect::kNone) {
    if (!probe_hdr_tiff_even_size(req.hdr_tiff, &master_w, &master_h, &err)) {
      if (error_out) *error_out = "HDR TIFF: " + err;
      std::cerr << "HDR TIFF: " << err << "\n";
      return 3;
    }
    unsigned dst_w = master_w;
    unsigned dst_h = master_h;
    encode_target_size(req, master_w, master_h, &dst_w, &dst_h);
    const unsigned hdr_dst_w = (req.gainmap_in.empty() && (dst_w != master_w || dst_h != master_h))
                                   ? dst_w
                                   : 0;
    const unsigned hdr_dst_h = hdr_dst_w ? dst_h : 0;
    if (!load_hdr_tiff_raw(req.hdr_tiff, &hdr, &err, 0, 0, nullptr, hdr_dst_w, hdr_dst_h)) {
      if (error_out) *error_out = "HDR TIFF: " + err;
      std::cerr << "HDR TIFF: " << err << "\n";
      return 3;
    }
    if (!req.gainmap_in.empty()) {
      if (!apply_gainmap_to_hdr(&hdr, req.base_path, req.gainmap_in, &err, master_w, master_h)) {
        if (error_out) *error_out = err;
        std::cerr << "gainmap: " << err << "\n";
        return 10;
      }
    }
    if (!load_sdr_base_raw(req.base_path, master_w, master_h, &sdr, &err)) {
      if (error_out) *error_out = "SDR base: " + err;
      std::cerr << "SDR base: " << err << "\n";
      return 4;
    }
    RawImageHolder hdr_out;
    RawImageHolder sdr_out;
    const RawImageHolder* hdr_enc = &hdr;
    const RawImageHolder* sdr_enc = &sdr;
    if (hdr.ref().w != dst_w || hdr.ref().h != dst_h) {
      if (!resize_hdr_lanczos(hdr, dst_w, dst_h, &hdr_out, &err)) {
        if (error_out) *error_out = err;
        return 5;
      }
      hdr_enc = &hdr_out;
    }
    if (sdr.ref().w != dst_w || sdr.ref().h != dst_h) {
      if (!resize_sdr_lanczos_sharpen(sdr, dst_w, dst_h, &sdr_out, &err)) {
        if (error_out) *error_out = err;
        return 5;
      }
      sdr_enc = &sdr_out;
    }
    EncodeOptions opt = req.options;
    if (!req.gainmap_in.empty()) opt.gainmap_in = req.gainmap_in;
    if (!req.watermark_config.empty()) opt.watermark_config = req.watermark_config;
    if (!req.metadata_patch.empty()) opt.metadata_patch = req.metadata_patch;
    if (!encode_pair(*hdr_enc, *sdr_enc, opt, req.out_path, &err)) {
      if (error_out) *error_out = err;
      return 5;
    }
    return 0;
  }

  if (req.slice_aspect == SliceAspect::kNone) {
    if (!load_hdr_tiff_raw(req.hdr_tiff, &hdr, &err)) {
      if (error_out) *error_out = "HDR TIFF: " + err;
      std::cerr << "HDR TIFF: " << err << "\n";
      return 3;
    }

    master_w = hdr.ref().w;
    master_h = hdr.ref().h;

    if (!load_sdr_base_raw(req.base_path, master_w, master_h, &sdr, &err)) {
      if (error_out) *error_out = "SDR base: " + err;
      std::cerr << "SDR base: " << err << "\n";
      return 4;
    }

    if (!req.gainmap_in.empty()) {
      if (!apply_gainmap_to_hdr(&hdr, req.base_path, req.gainmap_in, &err, master_w, master_h)) {
        if (error_out) *error_out = err;
        std::cerr << "gainmap: " << err << "\n";
        return 10;
      }
    }

    EncodeOptions opt = req.options;
    if (!req.gainmap_in.empty()) {
      opt.gainmap_in = req.gainmap_in;
    }
    if (!req.watermark_config.empty()) {
      opt.watermark_config = req.watermark_config;
    }
    if (!req.metadata_patch.empty()) {
      opt.metadata_patch = req.metadata_patch;
    }

    unsigned out_w = master_w;
    unsigned out_h = master_h;
    RawImageHolder hdr_out;
    RawImageHolder sdr_out;
    const RawImageHolder* hdr_enc = &hdr;
    const RawImageHolder* sdr_enc = &sdr;
    if (encode_target_size(req, hdr.ref().w, hdr.ref().h, &out_w, &out_h)) {
      if (hdr.ref().w != out_w || hdr.ref().h != out_h) {
        if (!resize_hdr_lanczos(hdr, out_w, out_h, &hdr_out, &err)) {
          if (error_out) *error_out = err;
          return 5;
        }
        hdr_enc = &hdr_out;
      }
      if (sdr.ref().w != out_w || sdr.ref().h != out_h) {
        if (!resize_sdr_lanczos_sharpen(sdr, out_w, out_h, &sdr_out, &err)) {
          if (error_out) *error_out = err;
          return 5;
        }
        sdr_enc = &sdr_out;
      }
    }

    if (!encode_pair(*hdr_enc, *sdr_enc, opt, req.out_path, &err)) {
      if (error_out) *error_out = err;
      return 5;
    }
    return 0;
  }

  if (!probe_hdr_tiff_even_size(req.hdr_tiff, &master_w, &master_h, &err)) {
    if (error_out) *error_out = "HDR TIFF: " + err;
    std::cerr << "HDR TIFF: " << err << "\n";
    return 3;
  }

  std::vector<CropRect> slices;
  if (!compute_slices(master_w, master_h, req.slice_aspect, &slices, &err, req.crop_offset,
                      req.slice_count)) {
    if (error_out) *error_out = "slice plan: " + err;
    std::cerr << "slice plan: " << err << "\n";
    return 6;
  }
  if (slices.empty()) {
    if (error_out) *error_out = "slice plan produced no tiles";
    return 6;
  }

  EncodeOptions opt = req.options;
  if (!req.gainmap_in.empty()) {
    opt.gainmap_in = req.gainmap_in;
  }
  if (!req.watermark_config.empty()) {
    opt.watermark_config = req.watermark_config;
  }
  if (!req.metadata_patch.empty()) {
    opt.metadata_patch = req.metadata_patch;
  }

  std::cerr << "Slicing " << master_w << "x" << master_h << " into " << slices.size() << " "
            << slice_aspect_label(req.slice_aspect) << " tile(s)\n";

  if (req.preview_slice_index > 0) {
    unsigned idx = req.preview_slice_index;
    if (idx > slices.size()) {
      idx = static_cast<unsigned>(slices.size());
    }
    const CropRect& crop = slices[idx - 1];
    if (!encode_crop(req, master_w, master_h, crop, opt, req.out_path, &err)) {
      if (error_out) *error_out = "preview slice: " + err;
      return 9;
    }
    return 0;
  }

  if (slices.size() == 1) {
    if (!encode_crop(req, master_w, master_h, slices[0], opt, req.out_path, &err)) {
      if (error_out) *error_out = err;
      return 9;
    }
    return 0;
  }

  unsigned idx = 1;
  std::string first_path;
  for (const auto& crop : slices) {
    const std::string slice_out = make_slice_output_path(req.out_path, req.slice_aspect, idx);
    std::cerr << "Slice " << idx << ": crop " << crop.x << "," << crop.y << " " << crop.w << "x"
              << crop.h << " -> " << slice_out << "\n";
    if (!encode_crop(req, master_w, master_h, crop, opt, slice_out, &err)) {
      if (error_out) *error_out = "slice " + std::to_string(idx) + ": " + err;
      return 9;
    }
    if (idx == 1) {
      first_path = slice_out;
    }
    ++idx;
  }

  if (!first_path.empty() && first_path != req.out_path) {
    std::error_code ec;
    fs::remove(fs::u8path(req.out_path), ec);
    fs::copy_file(fs::u8path(first_path), fs::u8path(req.out_path),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
      err = "Could not copy first slice to " + req.out_path + ": " + ec.message();
      if (error_out) *error_out = err;
      std::cerr << err << "\n";
      return 9;
    }
    std::cout << "Wrote " << req.out_path << " (first slice)\n";
  }
  return 0;
}

}  // namespace uhdr_repack
