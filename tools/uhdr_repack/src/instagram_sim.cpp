#include "instagram_sim.h"

#include "icc_profile.h"
#include "jpeg_container.h"
#include "jpeg_xmp_rights.h"
#include "path_io.h"

#include <ultrahdr_api.h>

#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

extern "C" {
#ifndef _WIN32
#ifndef HAVE_BOOLEAN
#define HAVE_BOOLEAN
typedef int boolean;
#endif
#endif
#include <jpeglib.h>
}

namespace uhdr_repack {

namespace {

struct JpegErr {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void jpeg_fail(j_common_ptr cinfo) { longjmp(reinterpret_cast<JpegErr*>(cinfo->err)->jump, 1); }

bool decode_jpeg_pixels(const uint8_t* data, size_t size, std::vector<uint8_t>* px, int* width, int* height,
                        int* comps, std::string* error) {
  jpeg_decompress_struct cinfo{};
  JpegErr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_fail;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_decompress(&cinfo);
    if (error) *error = "libjpeg failed to decode";
    return false;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, const_cast<unsigned char*>(data), static_cast<unsigned long>(size));
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    if (error) *error = "not a JPEG";
    return false;
  }
  cinfo.out_color_space = cinfo.num_components == 1 ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_start_decompress(&cinfo);
  *width = static_cast<int>(cinfo.output_width);
  *height = static_cast<int>(cinfo.output_height);
  *comps = cinfo.output_components;
  const size_t row_bytes = static_cast<size_t>(*width) * static_cast<size_t>(*comps);
  px->assign(row_bytes * static_cast<size_t>(*height), 0);
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW rows[1] = {px->data() + static_cast<size_t>(cinfo.output_scanline) * row_bytes};
    jpeg_read_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return true;
}

bool encode_jpeg_pixels(const std::vector<uint8_t>& px, int width, int height, int comps,
                        const std::vector<uint8_t>* icc, std::vector<uint8_t>* out, std::string* error) {
  jpeg_compress_struct cinfo{};
  JpegErr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_fail;
  unsigned char* buf = nullptr;
  unsigned long size = 0;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_compress(&cinfo);
    if (buf) free(buf);
    if (error) *error = "libjpeg failed to encode";
    return false;
  }
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &buf, &size);
  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = comps;
  cinfo.in_color_space = comps == 1 ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, kInstagramJpegQuality, TRUE);
  if (comps == 3) {
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 2;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;
  }
  jpeg_start_compress(&cinfo, TRUE);
  if (icc && !icc->empty()) {
    std::vector<uint8_t> marker;
    const char tag[] = "ICC_PROFILE";
    marker.insert(marker.end(), tag, tag + 12);
    marker.push_back(1);
    marker.push_back(1);
    marker.insert(marker.end(), icc->begin(), icc->end());
    jpeg_write_marker(&cinfo, JPEG_APP0 + 2, marker.data(), static_cast<unsigned int>(marker.size()));
  }
  const size_t row_bytes = static_cast<size_t>(width) * static_cast<size_t>(comps);
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW rows[1] = {const_cast<uint8_t*>(px.data()) + static_cast<size_t>(cinfo.next_scanline) * row_bytes};
    jpeg_write_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  if (!buf || size == 0) {
    if (buf) free(buf);
    if (error) *error = "libjpeg produced an empty JPEG";
    return false;
  }
  out->assign(buf, buf + size);
  free(buf);
  return true;
}

bool reencode(const uint8_t* data, size_t size, const std::vector<uint8_t>* icc, std::vector<uint8_t>* out,
              const char* what, std::string* error) {
  std::vector<uint8_t> px;
  int w = 0;
  int h = 0;
  int comps = 0;
  if (!decode_jpeg_pixels(data, size, &px, &w, &h, &comps, error) ||
      !encode_jpeg_pixels(px, w, h, comps, icc, out, error)) {
    if (error) *error = std::string(what) + ": " + *error;
    return false;
  }
  return true;
}

}  // namespace

std::string instagram_output_path(const std::string& jpg_path) {
  std::filesystem::path p = path_from_utf8(jpg_path);
  const std::string stem = p.stem().u8string();
  p.replace_filename(path_from_utf8(stem + "-instagram.jpg"));
  return p.u8string();
}

bool simulate_instagram(const std::string& in_path, const std::string& out_path, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!load_binary_file(in_path, &bytes, error)) return false;

  uhdr_codec_private_t* dec = uhdr_create_decoder();
  if (!dec) {
    if (error) *error = "uhdr_create_decoder failed";
    return false;
  }
  uhdr_compressed_image_t in{};
  in.data = bytes.data();
  in.data_sz = bytes.size();
  in.capacity = bytes.size();
  in.cg = UHDR_CG_UNSPECIFIED;
  in.ct = UHDR_CT_UNSPECIFIED;
  in.range = UHDR_CR_UNSPECIFIED;
  uhdr_error_info_t st = uhdr_dec_set_image(dec, &in);
  if (st.error_code == UHDR_CODEC_OK) st = uhdr_dec_probe(dec);
  uhdr_gainmap_metadata_t* meta_p = st.error_code == UHDR_CODEC_OK ? uhdr_dec_get_gainmap_metadata(dec) : nullptr;
  uhdr_mem_block_t* gm_p = st.error_code == UHDR_CODEC_OK ? uhdr_dec_get_gainmap_image(dec) : nullptr;
  if (!meta_p || !gm_p || !gm_p->data || gm_p->data_sz == 0) {
    uhdr_release_decoder(dec);
    if (error) *error = in_path + " is not an Ultra HDR JPEG";
    return false;
  }
  uhdr_gainmap_metadata_t meta = *meta_p;
  const std::vector<uint8_t> gm_in(static_cast<const uint8_t*>(gm_p->data),
                                   static_cast<const uint8_t*>(gm_p->data) + gm_p->data_sz);
  uhdr_release_decoder(dec);

  const std::vector<uint8_t> icc = build_display_p3_icc();
  std::vector<uint8_t> base;
  std::vector<uint8_t> gain;
  if (!reencode(bytes.data(), bytes.size(), &icc, &base, "base", error) ||
      !reencode(gm_in.data(), gm_in.size(), nullptr, &gain, "gain map", error)) {
    return false;
  }

  uhdr_codec_private_t* enc = uhdr_create_encoder();
  if (!enc) {
    if (error) *error = "uhdr_create_encoder failed";
    return false;
  }
  uhdr_compressed_image_t base_img{};
  base_img.data = base.data();
  base_img.data_sz = base.size();
  base_img.capacity = base.size();
  base_img.cg = UHDR_CG_DISPLAY_P3;
  base_img.ct = UHDR_CT_SRGB;
  base_img.range = UHDR_CR_FULL_RANGE;
  uhdr_compressed_image_t gain_img{};
  gain_img.data = gain.data();
  gain_img.data_sz = gain.size();
  gain_img.capacity = gain.size();
  gain_img.cg = UHDR_CG_UNSPECIFIED;
  gain_img.ct = UHDR_CT_UNSPECIFIED;
  gain_img.range = UHDR_CR_UNSPECIFIED;
  st = uhdr_enc_set_compressed_image(enc, &base_img, UHDR_BASE_IMG);
  if (st.error_code == UHDR_CODEC_OK) st = uhdr_enc_set_gainmap_image(enc, &gain_img, &meta);
  if (st.error_code == UHDR_CODEC_OK) st = uhdr_enc_set_output_format(enc, UHDR_CODEC_JPG);
  if (st.error_code == UHDR_CODEC_OK) st = uhdr_encode(enc);
  uhdr_compressed_image_t* out = st.error_code == UHDR_CODEC_OK ? uhdr_get_encoded_stream(enc) : nullptr;
  if (!out || !out->data || out->data_sz == 0) {
    uhdr_release_encoder(enc);
    if (error) *error = std::string("libultrahdr could not mux the re-encoded file") + (st.has_detail ? ": " : "") +
                        (st.has_detail ? st.detail : "");
    return false;
  }
  std::vector<uint8_t> jpeg_out(static_cast<const uint8_t*>(out->data),
                                static_cast<const uint8_t*>(out->data) + out->data_sz);
  if (!inject_sdr_xmp_rights(&jpeg_out, error)) {
    uhdr_release_encoder(enc);
    return false;
  }
  std::ofstream ofs = open_output_binary(out_path);
  if (!ofs) {
    uhdr_release_encoder(enc);
    if (error) *error = "could not open " + out_path;
    return false;
  }
  ofs.write(reinterpret_cast<const char*>(jpeg_out.data()), static_cast<std::streamsize>(jpeg_out.size()));
  uhdr_release_encoder(enc);
  if (!ofs) {
    if (error) *error = "could not write " + out_path;
    return false;
  }
  return true;
}

int cli_simulate_instagram_main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "--simulate-instagram requires <in.jpg> <out.jpg>\n";
    return 1;
  }
  std::string err;
  if (!simulate_instagram(argv[2], argv[3], &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  std::cout << "Wrote " << argv[3] << " (Instagram re-encode, q" << kInstagramJpegQuality << " 4:2:0)\n";
  return 0;
}

}  // namespace uhdr_repack
