#include "jpeg_xmp_rights.h"

#include <cstddef>
#include <cstring>
#include <string_view>

namespace uhdr_repack {

namespace {

constexpr uint8_t kSoi0 = 0xff;
constexpr uint8_t kSoi1 = 0xd8;
constexpr uint8_t kSos = 0xda;
constexpr uint8_t kEoi = 0xd9;
constexpr uint8_t kApp1 = 0xe1;
constexpr size_t kMaxAppPayload = 65533;
const char kXmpNs[] = "http://ns.adobe.com/xap/1.0/";

const char kUsageTermsXml[] =
    "<xmpRights:UsageTerms><rdf:Alt><rdf:li xml:lang=\"x-default\">"
    "https://github.com/karachungen/lightroom-plugin-export-hdr"
    "</rdf:li></rdf:Alt></xmpRights:UsageTerms>";

const char kDescAttrs[] =
    " xmlns:xmpRights=\"http://ns.adobe.com/xap/1.0/rights/\""
    " xmpRights:WebStatement=\"https://hdr.karachun.by/\"";

bool is_standalone_marker(uint8_t marker) {
  return marker == 0x01 || (marker >= 0xd0 && marker <= 0xd9);
}

bool find_primary_xmp_app1(const std::vector<uint8_t>& jpeg, size_t* marker_off, size_t* seg_len) {
  if (jpeg.size() < 4 || jpeg[0] != kSoi0 || jpeg[1] != kSoi1) {
    return false;
  }
  size_t i = 2;
  while (i + 1 < jpeg.size()) {
    if (jpeg[i] != 0xff) {
      ++i;
      continue;
    }
    while (i < jpeg.size() && jpeg[i] == 0xff) {
      ++i;
    }
    if (i >= jpeg.size()) {
      break;
    }
    const uint8_t marker = jpeg[i];
    ++i;
    if (marker == kSos || marker == kEoi) {
      return false;
    }
    if (is_standalone_marker(marker)) {
      continue;
    }
    if (i + 1 >= jpeg.size()) {
      return false;
    }
    const uint16_t length = static_cast<uint16_t>((jpeg[i] << 8) | jpeg[i + 1]);
    if (length < 2 || i + length > jpeg.size()) {
      return false;
    }
    if (marker == kApp1) {
      const uint8_t* payload = jpeg.data() + i + 2;
      const size_t payload_len = static_cast<size_t>(length) - 2;
      const size_t ns_len = sizeof(kXmpNs);  // includes trailing NUL
      if (payload_len >= ns_len && std::memcmp(payload, kXmpNs, ns_len) == 0) {
        *marker_off = i - 2;
        *seg_len = 2 + static_cast<size_t>(length);
        return true;
      }
    }
    i += length;
  }
  return false;
}

bool patch_rdf_description(std::string* xml, std::string* error) {
  if (xml->find(kXmpRightsWebStatement) != std::string::npos &&
      xml->find(kXmpRightsUsageTerms) != std::string::npos) {
    return true;
  }

  const size_t desc = xml->find("<rdf:Description");
  if (desc == std::string::npos) {
    if (error) *error = "primary XMP has no rdf:Description";
    return false;
  }
  const size_t tag_end = xml->find('>', desc);
  if (tag_end == std::string::npos) {
    if (error) *error = "primary XMP rdf:Description is not closed";
    return false;
  }

  const bool self_closing = tag_end > desc && (*xml)[tag_end - 1] == '/';
  std::string attrs = kDescAttrs;
  if (xml->find("xmlns:xmpRights=") != std::string::npos) {
    attrs = " xmpRights:WebStatement=\"https://hdr.karachun.by/\"";
  }

  if (self_closing) {
    xml->replace(tag_end - 1, 2, attrs + ">" + kUsageTermsXml + "</rdf:Description>");
    return true;
  }

  xml->insert(tag_end, attrs);
  const size_t close = xml->find("</rdf:Description>", tag_end);
  if (close == std::string::npos) {
    if (error) *error = "primary XMP rdf:Description has no end tag";
    return false;
  }
  xml->insert(close, kUsageTermsXml);
  return true;
}

}  // namespace

bool inject_sdr_xmp_rights(std::vector<uint8_t>* jpeg, std::string* error) {
  if (!jpeg) {
    if (error) *error = "null JPEG buffer";
    return false;
  }

  size_t marker_off = 0;
  size_t seg_len = 0;
  if (!find_primary_xmp_app1(*jpeg, &marker_off, &seg_len)) {
    if (error) *error = "primary APP1 XMP packet not found";
    return false;
  }

  const size_t length_off = marker_off + 2;
  const uint16_t length =
      static_cast<uint16_t>(((*jpeg)[length_off] << 8) | (*jpeg)[length_off + 1]);
  const size_t ns_len = sizeof(kXmpNs);
  const size_t xml_off = length_off + 2 + ns_len;
  const size_t xml_len = static_cast<size_t>(length) - 2 - ns_len;
  if (xml_off + xml_len > jpeg->size()) {
    if (error) *error = "primary XMP packet overruns JPEG";
    return false;
  }

  std::string xml(reinterpret_cast<const char*>(jpeg->data() + xml_off), xml_len);
  if (!patch_rdf_description(&xml, error)) {
    return false;
  }

  const size_t new_payload = ns_len + xml.size();
  if (new_payload + 2 > kMaxAppPayload) {
    if (error) *error = "primary XMP APP1 would exceed 64 KiB";
    return false;
  }
  const uint16_t new_length = static_cast<uint16_t>(2 + new_payload);
  const size_t old_seg = 2 + static_cast<size_t>(length);
  const size_t new_seg = 2 + static_cast<size_t>(new_length);
  if (new_seg == old_seg &&
      std::string_view(reinterpret_cast<const char*>(jpeg->data() + xml_off), xml_len) == xml) {
    return true;
  }

  std::vector<uint8_t> out;
  out.reserve(jpeg->size() + (new_seg - old_seg));
  out.insert(out.end(), jpeg->begin(), jpeg->begin() + static_cast<std::ptrdiff_t>(marker_off));
  out.push_back(0xff);
  out.push_back(kApp1);
  out.push_back(static_cast<uint8_t>((new_length >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(new_length & 0xff));
  out.insert(out.end(), reinterpret_cast<const uint8_t*>(kXmpNs),
             reinterpret_cast<const uint8_t*>(kXmpNs) + ns_len);
  out.insert(out.end(), reinterpret_cast<const uint8_t*>(xml.data()),
             reinterpret_cast<const uint8_t*>(xml.data()) + xml.size());
  out.insert(out.end(), jpeg->begin() + static_cast<std::ptrdiff_t>(marker_off + old_seg),
             jpeg->end());
  *jpeg = std::move(out);
  return true;
}

}  // namespace uhdr_repack
