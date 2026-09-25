#include "chart_io.h"

#include "path_io.h"

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
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

constexpr int kStripRows = 16;

struct JpegErr {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void jpeg_fail(j_common_ptr cinfo) { longjmp(reinterpret_cast<JpegErr*>(cinfo->err)->jump, 1); }

void put_u16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = static_cast<uint8_t>(v);
  b[at + 1] = static_cast<uint8_t>(v >> 8);
}

void put_u32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  b[at] = static_cast<uint8_t>(v);
  b[at + 1] = static_cast<uint8_t>(v >> 8);
  b[at + 2] = static_cast<uint8_t>(v >> 16);
  b[at + 3] = static_cast<uint8_t>(v >> 24);
}

void apply_float_predictor(uint8_t* row, size_t values, uint32_t stride) {
  const size_t bytes = values * 4u;
  std::vector<uint8_t> planes(bytes);
  for (size_t v = 0; v < values; ++v) {
    for (size_t b = 0; b < 4; ++b) planes[(3 - b) * values + v] = row[v * 4u + b];
  }
  for (size_t i = bytes; i-- > stride;) planes[i] = static_cast<uint8_t>(planes[i] - planes[i - stride]);
  std::memcpy(row, planes.data(), bytes);
}

}  // namespace

LinearRgb sdr_p3(LinearRgb rec) {
  rec.r = std::max(0.0f, rec.r);
  rec.g = std::max(0.0f, rec.g);
  rec.b = std::max(0.0f, rec.b);
  // Clip after the gamut conversion. Scaling the Rec.2020 vector down first shrinks the small
  // positive P3 channel of an out-of-gamut primary (Rec.2020 red's P3 blue) while the HDR value
  // does not, so that channel's gain hits the boost ceiling and the gain map is no longer a primary.
  LinearRgb p3 = rec2020_to_display_p3(rec);
  p3.r = std::clamp(p3.r, 0.0f, 1.0f);
  p3.g = std::clamp(p3.g, 0.0f, 1.0f);
  p3.b = std::clamp(p3.b, 0.0f, 1.0f);
  return p3;
}

bool write_float_tiff(const std::string& path, const std::vector<float>& rgb, int width, int height,
                      const std::vector<uint8_t>& icc, std::string* error) {
  if (width < 1 || height < 1) {
    if (error) *error = "TIFF width and height must be positive";
    return false;
  }
  const uint32_t nstrips =
      static_cast<uint32_t>((height + kStripRows - 1) / kStripRows);
  const size_t row_values = static_cast<size_t>(width) * 3u;
  if (rgb.size() < static_cast<size_t>(height) * row_values) {
    if (error) *error = "internal: HDR buffer does not cover the TIFF";
    return false;
  }
  std::vector<std::vector<uint8_t>> strips(nstrips);
  for (uint32_t s = 0; s < nstrips; ++s) {
    const int rows_in_strip = std::min(kStripRows, height - static_cast<int>(s) * kStripRows);
    const size_t strip_raw = static_cast<size_t>(rows_in_strip) * row_values * 4u;
    std::vector<uint8_t> raw(strip_raw);
    std::memcpy(raw.data(), rgb.data() + static_cast<size_t>(s) * kStripRows * row_values, strip_raw);
    for (int r = 0; r < rows_in_strip; ++r) {
      apply_float_predictor(raw.data() + static_cast<size_t>(r) * row_values * 4u, row_values, 3);
    }
    mz_ulong len = mz_compressBound(static_cast<mz_ulong>(strip_raw));
    strips[s].resize(len);
    if (mz_compress2(strips[s].data(), &len, raw.data(), static_cast<mz_ulong>(strip_raw), 6) != MZ_OK) {
      if (error) *error = "Deflate failed on TIFF strip " + std::to_string(s);
      return false;
    }
    strips[s].resize(len);
  }

  constexpr uint32_t ntags = 18;
  constexpr uint32_t ifd = 8;
  constexpr uint32_t after = ifd + 2 + ntags * 12 + 4;
  uint32_t cursor = (after + 3u) & ~3u;
  const uint32_t bits_off = cursor;
  cursor += 8;
  const uint32_t fmt_off = cursor;
  cursor += 8;
  const uint32_t xres_off = cursor;
  cursor += 8;
  const uint32_t yres_off = cursor;
  cursor += 8;
  const char* software = "uhdr_repack";
  const uint32_t software_len = static_cast<uint32_t>(std::strlen(software) + 1);
  const uint32_t soft_off = cursor;
  cursor += software_len;
  cursor = (cursor + 3u) & ~3u;
  const uint32_t strips_off = cursor;
  cursor += nstrips * 4u;
  const uint32_t counts_off = cursor;
  cursor += nstrips * 4u;
  const uint32_t icc_off = cursor;
  cursor += static_cast<uint32_t>(icc.size());
  cursor = (cursor + 3u) & ~3u;
  const uint32_t data_off = cursor;
  size_t total = data_off;
  for (const auto& strip : strips) total += strip.size();

  std::vector<uint8_t> file(total, 0);
  file[0] = 'I';
  file[1] = 'I';
  put_u16(file, 2, 42);
  put_u32(file, 4, ifd);
  put_u16(file, ifd, static_cast<uint16_t>(ntags));
  size_t e = ifd + 2;
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
    put_u16(file, e, tag);
    put_u16(file, e + 2, type);
    put_u32(file, e + 4, count);
    put_u32(file, e + 8, value);
    e += 12;
  };
  entry(256, 3, 1, static_cast<uint32_t>(width));
  entry(257, 3, 1, static_cast<uint32_t>(height));
  entry(258, 3, 3, bits_off);
  entry(259, 3, 1, 8);
  entry(262, 3, 1, 2);
  entry(273, 4, nstrips, strips_off);
  entry(274, 3, 1, 1);
  entry(277, 3, 1, 3);
  entry(278, 3, 1, static_cast<uint32_t>(kStripRows));
  entry(279, 4, nstrips, counts_off);
  entry(282, 5, 1, xres_off);
  entry(283, 5, 1, yres_off);
  entry(284, 3, 1, 1);
  entry(296, 3, 1, 2);
  entry(305, 2, software_len, soft_off);
  entry(317, 3, 1, 3);
  entry(339, 3, 3, fmt_off);
  entry(34675, 7, static_cast<uint32_t>(icc.size()), icc_off);
  put_u32(file, e, 0);
  put_u16(file, bits_off, 32);
  put_u16(file, bits_off + 2, 32);
  put_u16(file, bits_off + 4, 32);
  put_u16(file, fmt_off, 3);
  put_u16(file, fmt_off + 2, 3);
  put_u16(file, fmt_off + 4, 3);
  put_u32(file, xres_off, 72);
  put_u32(file, xres_off + 4, 1);
  put_u32(file, yres_off, 72);
  put_u32(file, yres_off + 4, 1);
  std::memcpy(file.data() + soft_off, software, software_len);
  std::memcpy(file.data() + icc_off, icc.data(), icc.size());
  uint32_t at = data_off;
  for (uint32_t s = 0; s < nstrips; ++s) {
    put_u32(file, strips_off + s * 4u, at);
    put_u32(file, counts_off + s * 4u, static_cast<uint32_t>(strips[s].size()));
    std::memcpy(file.data() + at, strips[s].data(), strips[s].size());
    at += static_cast<uint32_t>(strips[s].size());
  }

  std::ofstream out = open_output_binary(path);
  if (!out) {
    if (error) *error = "could not write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  if (!out) {
    if (error) *error = "could not finish " + path;
    return false;
  }
  return true;
}

bool encode_p3_jpeg(const uint8_t* rgba, int width, int height, const std::vector<uint8_t>& icc,
                    std::vector<uint8_t>* jpeg, std::string* error) {
  jpeg_compress_struct cinfo{};
  JpegErr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_fail;
  unsigned char* outbuf = nullptr;
  unsigned long outsize = 0;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_compress(&cinfo);
    if (outbuf) free(outbuf);
    if (error) *error = "libjpeg failed to compress the chart JPEG";
    return false;
  }
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &outbuf, &outsize);
  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 95, TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  std::vector<uint8_t> marker;
  const char hdr[] = "ICC_PROFILE";
  marker.insert(marker.end(), hdr, hdr + 12);
  marker.push_back(1);
  marker.push_back(1);
  marker.insert(marker.end(), icc.begin(), icc.end());
  jpeg_write_marker(&cinfo, JPEG_APP0 + 2, marker.data(), static_cast<unsigned int>(marker.size()));
  std::vector<uint8_t> row(static_cast<size_t>(width) * 3u);
  while (cinfo.next_scanline < cinfo.image_height) {
    const uint8_t* src = rgba + static_cast<size_t>(cinfo.next_scanline) * width * 4u;
    for (int x = 0; x < width; ++x) {
      row[static_cast<size_t>(x) * 3u] = src[static_cast<size_t>(x) * 4u];
      row[static_cast<size_t>(x) * 3u + 1] = src[static_cast<size_t>(x) * 4u + 1];
      row[static_cast<size_t>(x) * 3u + 2] = src[static_cast<size_t>(x) * 4u + 2];
    }
    JSAMPROW rows[1] = {row.data()};
    jpeg_write_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  if (!outbuf || outsize == 0) {
    if (error) *error = "libjpeg produced an empty chart JPEG";
    return false;
  }
  jpeg->assign(outbuf, outbuf + outsize);
  free(outbuf);
  return true;
}

bool write_bytes(const std::string& path, const std::vector<uint8_t>& bytes, std::string* error) {
  std::ofstream out = open_output_binary(path);
  if (!out) {
    if (error) *error = "could not write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

}  // namespace uhdr_repack
