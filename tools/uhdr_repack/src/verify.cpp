#include "verify.h"

#include <ultrahdr_api.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace uhdr_repack {

namespace {

bool ParseSofSamplingFactors(const uint8_t* data, size_t len, std::string* detail_out,
                             bool* looks_like_420) {
  *looks_like_420 = false;
  size_t i = 0;
  while (i + 1 < len) {
    if (data[i] != 0xff) {
      ++i;
      continue;
    }
    uint8_t marker = data[i + 1];
    if (marker >= 0xc0 && marker <= 0xc3) {
      if (i + 4 > len) return false;
      uint16_t seglen = (uint16_t)((data[i + 2] << 8) | data[i + 3]);
      if (i + seglen > len) return false;
      const       uint8_t ncomp = data[i + 9];
      if (ncomp < 1 || ncomp > 4) return false;
      if (i + 10 + (size_t)ncomp * 3 > len) return false;
      uint8_t maxh = 0, maxv = 0;
      std::ostringstream oss;
      oss << "SOF components=" << int(ncomp);
      for (unsigned c = 0; c < ncomp; ++c) {
        /* Baseline SOF: per component id (1B), sampling (1B H|V), qtable (1B) */
        uint8_t hv = data[i + 11 + c * 3];
        uint8_t h = (hv >> 4) & 0x0f;
        uint8_t v = hv & 0x0f;
        maxh = maxh > h ? maxh : h;
        maxv = maxv > v ? maxv : v;
        oss << " [" << c << "]h=" << int(h) << "v=" << int(v);
      }
      // Very loose 4:2:0 heuristic: chroma planes subsampled vs max factors.
      if (ncomp >= 3) {
        uint8_t h0 = (data[i + 11] >> 4) & 0x0f;
        uint8_t v0 = data[i + 11] & 0x0f;
        uint8_t h1 = (data[i + 14] >> 4) & 0x0f;
        uint8_t v1 = data[i + 14] & 0x0f;
        uint8_t h2 = (data[i + 17] >> 4) & 0x0f;
        uint8_t v2 = data[i + 17] & 0x0f;
        if (h0 >= 2 && v0 >= 2 && h1 == 1 && v1 == 1 && h2 == 1 && v2 == 1) {
          *looks_like_420 = true;
        }
        if (h0 == 2 && v0 == 1 && h1 == 1 && v1 == 1 && h2 == 1 && v2 == 1) {
          *looks_like_420 = true;
        }
      }
      if (detail_out) *detail_out += oss.str();
      return true;
    }
    // Skip variable-length marker segments.
    if (marker >= 0xd0 && marker <= 0xd7) {
      i += 2;
      continue;
    }
    if (marker == 0xd8 || marker == 0xd9) {
      i += 2;
      continue;
    }
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd9)) {
      i += 2;
      continue;
    }
    if (i + 4 > len) break;
    uint16_t seglen = (uint16_t)((data[i + 2] << 8) | data[i + 3]);
    if (seglen < 2) break;
    i += 2 + seglen;
  }
  return false;
}

bool ScanMarkers(const uint8_t* data, size_t len, InspectReport* report) {
  bool mpf = false;
  bool iso = false;
  bool xmp = false;
  if (len > 32) {
    std::string_view blob(reinterpret_cast<const char*>(data), len);
    if (blob.find("GContainer") != std::string_view::npos ||
        blob.find("hdrgm:") != std::string_view::npos ||
        blob.find("xmlns:hdrgm") != std::string_view::npos) {
      xmp = true;
    }
  }
  size_t i = 0;
  while (i + 3 < len) {
    if (data[i] == 0xff && data[i + 1] == 0xe2) {
      uint16_t seglen = (uint16_t)((data[i + 2] << 8) | data[i + 3]);
      if (i + 4 + 10 <= len) {
        const char* sig = reinterpret_cast<const char*>(data + i + 4);
        if (std::strncmp(sig, "ICC_PROFILE", 11) == 0) {
          i += 2 + seglen;
          continue;
        }
      }
      if (i + 6 <= len && data[i + 4] == 'M' && data[i + 5] == 'P' && data[i + 6] == 'F') {
        mpf = true;
      }
      // ISO 21496-1 APP2 often starts with length then identifier string.
      if (i + 10 < len) {
        const char* p = reinterpret_cast<const char*>(data + i + 4);
        if (std::strstr(p, "urn:iso:std:iso:ts:21496:-1") != nullptr ||
            std::strstr(p, "ISO21496") != nullptr) {
          iso = true;
        }
      }
    }
    if (data[i] == 0xff && data[i + 1] == 0xe1) {
      uint16_t seglen = (uint16_t)((data[i + 2] << 8) | data[i + 3]);
      if (i + 4 + 29 <= len) {
        const char* p = reinterpret_cast<const char*>(data + i + 4);
        if (std::strncmp(p, "http://ns.adobe.com/xap", 23) == 0 ||
            std::strstr(p, "hdrgm") != nullptr) {
          xmp = true;
        }
      }
      (void)seglen;
    }
    ++i;
  }
  report->has_mpf = mpf;
  report->has_iso_app2 = iso;
  report->has_primary_xmp = xmp;
  return true;
}

}  // namespace

bool inspect_ultra_hdr_file(const std::string& path, InspectReport* report, std::string* error) {
  if (!report) {
    if (error) *error = "null report";
    return false;
  }
  *report = InspectReport{};

  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    if (error) *error = "Cannot read file: " + path;
    return false;
  }
  std::vector<uint8_t> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  if (buf.empty()) {
    if (error) *error = "Empty file";
    return false;
  }

  report->is_ultra_hdr =
      is_uhdr_image(buf.data(), static_cast<int>(buf.size())) != 0;

  ScanMarkers(buf.data(), buf.size(), report);

  std::string sof_detail;
  bool primary420 = false;
  ParseSofSamplingFactors(buf.data(), buf.size(), &sof_detail, &primary420);
  report->primary_jpeg_420 = primary420;
  report->detail = std::move(sof_detail);

  uhdr_codec_private_t* dec = uhdr_create_decoder();
  if (!dec) {
    return true;
  }

  uhdr_compressed_image_t uhdr{};
  uhdr.data = buf.data();
  uhdr.data_sz = buf.size();
  uhdr.capacity = buf.size();
  uhdr.cg = UHDR_CG_UNSPECIFIED;
  uhdr.ct = UHDR_CT_UNSPECIFIED;
  uhdr.range = UHDR_CR_UNSPECIFIED;

  uhdr_error_info_t st = uhdr_dec_set_image(dec, &uhdr);
  if (st.error_code != UHDR_CODEC_OK) {
    uhdr_release_decoder(dec);
    return true;
  }

  st = uhdr_dec_probe(dec);
  if (st.error_code != UHDR_CODEC_OK) {
    uhdr_release_decoder(dec);
    return true;
  }

  report->width = uhdr_dec_get_image_width(dec);
  report->height = uhdr_dec_get_image_height(dec);
  report->gainmap_width = uhdr_dec_get_gainmap_width(dec);
  report->gainmap_height = uhdr_dec_get_gainmap_height(dec);

  uhdr_mem_block_t* base = uhdr_dec_get_base_image(dec);
  if (base && base->data && base->data_sz > 0) {
    std::string d2;
    ParseSofSamplingFactors(static_cast<const uint8_t*>(base->data), base->data_sz, &d2,
                           &report->primary_jpeg_420);
    report->detail += " | base:" + d2;
  }

  uhdr_mem_block_t* gm = uhdr_dec_get_gainmap_image(dec);
  if (gm && gm->data && gm->data_sz > 0) {
    std::string d3;
    ParseSofSamplingFactors(static_cast<const uint8_t*>(gm->data), gm->data_sz, &d3,
                           &report->gainmap_jpeg_420);
    report->detail += " | gainmap:" + d3;
  }

  uhdr_gainmap_metadata_t* meta = uhdr_dec_get_gainmap_metadata(dec);
  if (meta) {
    std::ostringstream oss;
    oss << " min_boost=(" << meta->min_content_boost[0] << "," << meta->min_content_boost[1] << ","
        << meta->min_content_boost[2] << ")";
    report->detail += oss.str();
  }

  uhdr_release_decoder(dec);
  return true;
}

}  // namespace uhdr_repack
