#include "icc_profile.h"

#include "color_primaries.h"
#include "path_io.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace uhdr_repack {
namespace {

constexpr float kD65[3] = {0.95047f, 1.0f, 1.08883f};
constexpr float kD50[3] = {0.9642f, 1.0f, 0.8249f};

constexpr float kRec2020ToXyzD65[9] = {0.63695805f, 0.14461690f, 0.16888098f, 0.26270021f, 0.67799807f,
                                       0.05930172f, 0.00000000f, 0.02807269f, 1.06098506f};

constexpr float kDisplayP3ToXyzD65[9] = {0.48657095f, 0.26566769f, 0.19821729f, 0.22897456f, 0.69173852f,
                                         0.07928691f, 0.00000000f, 0.04511338f, 1.04394437f};

constexpr float kSrgbToXyzD65[9] = {0.41239080f, 0.35758434f, 0.18048079f, 0.21263901f, 0.71516868f,
                                    0.07219232f, 0.01933082f, 0.11919478f, 0.95053215f};

void append_be16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}

void append_be32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v >> 24));
  b.push_back(static_cast<uint8_t>(v >> 16));
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}

void append_s15(std::vector<uint8_t>& b, float v) {
  const auto q = static_cast<int32_t>(std::lround(static_cast<double>(v) * 65536.0));
  append_be32(b, static_cast<uint32_t>(q));
}

void write_be16_at(std::vector<uint8_t>& b, uint32_t at, uint16_t v) {
  b[at] = static_cast<uint8_t>(v >> 8);
  b[at + 1] = static_cast<uint8_t>(v);
}

uint16_t read_be16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

float read_s15(const uint8_t* p) {
  const auto v = static_cast<int32_t>(read_be32(p));
  return static_cast<float>(v) / 65536.0f;
}

void mul3(const float a[9], const float b[9], float o[9]) {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    }
  }
}

void bradford_d65_to_d50(const float rgb_to_xyz_d65[9], float out[9]) {
  const float bradford[9] = {0.8951f, 0.2664f, -0.1614f, -0.7502f, 1.7135f, 0.0367f, 0.0389f,
                             -0.0685f, 1.0296f};
  const float bradford_inv[9] = {0.9869929f, -0.1470543f, 0.1599627f, 0.4323053f, 0.5183603f,
                                 0.0492912f, -0.0085287f, 0.0400428f, 0.9684867f};
  float src_lms[3] = {bradford[0] * kD65[0] + bradford[1] * kD65[1] + bradford[2] * kD65[2],
                      bradford[3] * kD65[0] + bradford[4] * kD65[1] + bradford[5] * kD65[2],
                      bradford[6] * kD65[0] + bradford[7] * kD65[1] + bradford[8] * kD65[2]};
  float dst_lms[3] = {bradford[0] * kD50[0] + bradford[1] * kD50[1] + bradford[2] * kD50[2],
                      bradford[3] * kD50[0] + bradford[4] * kD50[1] + bradford[5] * kD50[2],
                      bradford[6] * kD50[0] + bradford[7] * kD50[1] + bradford[8] * kD50[2]};
  float scale[9] = {dst_lms[0] / src_lms[0], 0, 0, 0, dst_lms[1] / src_lms[1], 0, 0, 0,
                    dst_lms[2] / src_lms[2]};
  float tmp[9];
  float adapt[9];
  mul3(scale, bradford, tmp);
  mul3(bradford_inv, tmp, adapt);
  mul3(adapt, rgb_to_xyz_d65, out);
}

std::vector<uint8_t> xyz_type(float x, float y, float z) {
  std::vector<uint8_t> b;
  const char t[] = {'X', 'Y', 'Z', ' '};
  b.insert(b.end(), t, t + 4);
  append_be32(b, 0);
  append_s15(b, x);
  append_s15(b, y);
  append_s15(b, z);
  return b;
}

std::vector<uint8_t> curv_linear() {
  std::vector<uint8_t> b;
  const char t[] = {'c', 'u', 'r', 'v'};
  b.insert(b.end(), t, t + 4);
  append_be32(b, 0);
  append_be32(b, 0);
  return b;
}

std::vector<uint8_t> curv_srgb() {
  std::vector<uint8_t> b;
  const char t[] = {'c', 'u', 'r', 'v'};
  b.insert(b.end(), t, t + 4);
  append_be32(b, 0);
  append_be32(b, 1024);
  for (int i = 0; i < 1024; ++i) {
    const float x = static_cast<float>(i) / 1023.0f;
    const float y = std::clamp(srgb_eotf(x), 0.0f, 1.0f);
    append_be16(b, static_cast<uint16_t>(std::lround(y * 65535.0f)));
  }
  return b;
}

std::vector<uint8_t> text_desc(const char* ascii) {
  std::vector<uint8_t> b;
  const char t[] = {'d', 'e', 's', 'c'};
  b.insert(b.end(), t, t + 4);
  append_be32(b, 0);
  const auto n = static_cast<uint32_t>(std::strlen(ascii) + 1);
  append_be32(b, n);
  b.insert(b.end(), ascii, ascii + n);
  append_be32(b, 0);
  append_be32(b, 0);
  append_be16(b, 0);
  b.push_back(0);
  b.insert(b.end(), 67, 0);
  return b;
}

std::vector<uint8_t> text_type(const char* ascii) {
  std::vector<uint8_t> b;
  const char t[] = {'t', 'e', 'x', 't'};
  b.insert(b.end(), t, t + 4);
  append_be32(b, 0);
  const auto n = std::strlen(ascii) + 1;
  b.insert(b.end(), ascii, ascii + n);
  return b;
}

struct TagBlob {
  uint32_t sig = 0;
  int blob = 0;
};

std::vector<uint8_t> build_profile(const char* description, const float rgb_to_xyz_d65[9], bool linear) {
  float m[9];
  bradford_d65_to_d50(rgb_to_xyz_d65, m);
  const float white_x = m[0] + m[1] + m[2];
  const float white_y = m[3] + m[4] + m[5];
  const float white_z = m[6] + m[7] + m[8];

  std::vector<std::vector<uint8_t>> blobs;
  blobs.push_back(text_type("CC0"));
  blobs.push_back(text_desc(description));
  blobs.push_back(xyz_type(white_x, white_y, white_z));
  blobs.push_back(xyz_type(m[0], m[3], m[6]));
  blobs.push_back(xyz_type(m[1], m[4], m[7]));
  blobs.push_back(xyz_type(m[2], m[5], m[8]));
  blobs.push_back(linear ? curv_linear() : curv_srgb());

  const TagBlob tags[] = {
      {0x62545243u, 6}, {0x6258595Au, 5}, {0x63707274u, 0}, {0x64657363u, 1}, {0x67545243u, 6},
      {0x6758595Au, 4}, {0x72545243u, 6}, {0x7258595Au, 3}, {0x77747074u, 2},
  };
  constexpr int kTags = 9;
  constexpr uint32_t kHeader = 128;
  const uint32_t table = kHeader + 4 + static_cast<uint32_t>(kTags) * 12;
  uint32_t cursor = table;
  uint32_t blob_off[7];
  for (int i = 0; i < 7; ++i) {
    blob_off[i] = cursor;
    cursor += static_cast<uint32_t>(blobs[static_cast<size_t>(i)].size());
    cursor = (cursor + 3u) & ~3u;
  }

  std::vector<uint8_t> icc(cursor, 0);
  auto w32 = [&](uint32_t at, uint32_t v) {
    icc[at] = static_cast<uint8_t>(v >> 24);
    icc[at + 1] = static_cast<uint8_t>(v >> 16);
    icc[at + 2] = static_cast<uint8_t>(v >> 8);
    icc[at + 3] = static_cast<uint8_t>(v);
  };
  w32(0, cursor);
  w32(8, 0x02100000u);
  icc[12] = 'm';
  icc[13] = 'n';
  icc[14] = 't';
  icc[15] = 'r';
  icc[16] = 'R';
  icc[17] = 'G';
  icc[18] = 'B';
  icc[19] = ' ';
  icc[20] = 'X';
  icc[21] = 'Y';
  icc[22] = 'Z';
  icc[23] = ' ';
  write_be16_at(icc, 24, 2026);
  write_be16_at(icc, 26, 9);
  write_be16_at(icc, 28, 24);
  icc[36] = 'a';
  icc[37] = 'c';
  icc[38] = 's';
  icc[39] = 'p';
  w32(68, 0x0000F6D6u);
  w32(72, 0x00010000u);
  w32(76, 0x0000D32Du);
  w32(128, static_cast<uint32_t>(kTags));
  for (int i = 0; i < kTags; ++i) {
    const uint32_t e = 132u + static_cast<uint32_t>(i) * 12u;
    const auto& blob = blobs[static_cast<size_t>(tags[i].blob)];
    w32(e, tags[i].sig);
    w32(e + 4, blob_off[tags[i].blob]);
    w32(e + 8, static_cast<uint32_t>(blob.size()));
  }
  for (int i = 0; i < 7; ++i) {
    std::memcpy(icc.data() + blob_off[i], blobs[static_cast<size_t>(i)].data(),
                blobs[static_cast<size_t>(i)].size());
  }
  return icc;
}

bool fail(std::string* error, const std::string& msg) {
  if (error) *error = msg;
  return false;
}

bool validate_profile(const std::vector<uint8_t>& icc, const char* description, bool linear,
                      const float rgb_to_xyz_d65[9], std::string* error) {
  if (icc.size() < 128 || (icc.size() % 4u) != 0) {
    return fail(error, "ICC profile size is not a multiple of 4");
  }
  if (read_be32(icc.data()) != static_cast<uint32_t>(icc.size())) {
    return fail(error, "ICC profile size field does not match the buffer");
  }
  if (read_be32(icc.data() + 8) != 0x02100000u) return fail(error, "ICC version is not 2.1.0");
  if (std::memcmp(icc.data() + 12, "mntr", 4) != 0) return fail(error, "ICC device class is not mntr");
  if (std::memcmp(icc.data() + 16, "RGB ", 4) != 0) return fail(error, "ICC color space is not RGB");
  if (std::memcmp(icc.data() + 20, "XYZ ", 4) != 0) return fail(error, "ICC PCS is not XYZ");
  if (std::memcmp(icc.data() + 36, "acsp", 4) != 0) return fail(error, "ICC signature acsp is missing");
  if (read_be32(icc.data() + 68) != 0x0000F6D6u || read_be32(icc.data() + 72) != 0x00010000u ||
      read_be32(icc.data() + 76) != 0x0000D32Du) {
    return fail(error, "ICC header illuminant is not D50");
  }

  const uint32_t ntags = read_be32(icc.data() + 128);
  if (ntags < 9 || 132u + ntags * 12u > icc.size()) return fail(error, "ICC tag table is truncated");

  auto find = [&](uint32_t sig, uint32_t* tag_off, uint32_t* tag_sz) -> bool {
    for (uint32_t i = 0; i < ntags; ++i) {
      const uint8_t* e = icc.data() + 132u + i * 12u;
      if (read_be32(e) == sig) {
        *tag_off = read_be32(e + 4);
        *tag_sz = read_be32(e + 8);
        return true;
      }
    }
    return false;
  };

  uint32_t off = 0;
  uint32_t sz = 0;
  uint32_t prev = 0;
  for (uint32_t i = 0; i < ntags; ++i) {
    const uint8_t* e = icc.data() + 132u + i * 12u;
    const uint32_t s = read_be32(e);
    if (i > 0 && s <= prev) return fail(error, "ICC tag table is not sorted");
    prev = s;
    const uint32_t o = read_be32(e + 4);
    const uint32_t n = read_be32(e + 8);
    if (n == 0 || static_cast<size_t>(o) + n > icc.size()) return fail(error, "ICC tag data is out of range");
  }

  if (!find(0x64657363u, &off, &sz) || sz < 12 || std::memcmp(icc.data() + off, "desc", 4) != 0) {
    return fail(error, "ICC desc tag is missing");
  }
  const uint32_t ascii_n = read_be32(icc.data() + off + 8);
  if (ascii_n < 2 || 12u + ascii_n + 78u > sz) return fail(error, "ICC desc tag is truncated");
  if (icc[off + 12 + ascii_n - 1] != 0) return fail(error, "ICC desc ASCII is not terminated");
  const std::string ascii(reinterpret_cast<const char*>(icc.data() + off + 12), ascii_n - 1);
  if (ascii.find(description) == std::string::npos) {
    return fail(error, "ICC description does not contain " + std::string(description));
  }
  if (read_be32(icc.data() + off + 12 + ascii_n) != 0 ||
      read_be32(icc.data() + off + 16 + ascii_n) != 0) {
    return fail(error, "ICC desc Unicode block is not empty");
  }

  if (!find(0x63707274u, &off, &sz) || sz < 9 || std::memcmp(icc.data() + off, "text", 4) != 0) {
    return fail(error, "ICC cprt tag is missing");
  }

  auto read_xyz = [&](uint32_t sig, float xyz[3]) -> bool {
    uint32_t o = 0;
    uint32_t n = 0;
    if (!find(sig, &o, &n) || n < 20 || std::memcmp(icc.data() + o, "XYZ ", 4) != 0) return false;
    xyz[0] = read_s15(icc.data() + o + 8);
    xyz[1] = read_s15(icc.data() + o + 12);
    xyz[2] = read_s15(icc.data() + o + 16);
    return true;
  };

  float wtpt[3];
  float r[3];
  float g[3];
  float b[3];
  if (!read_xyz(0x77747074u, wtpt) || !read_xyz(0x7258595Au, r) || !read_xyz(0x6758595Au, g) ||
      !read_xyz(0x6258595Au, b)) {
    return fail(error, "ICC is missing wtpt or colorant tags");
  }
  const float sum[3] = {r[0] + g[0] + b[0], r[1] + g[1] + b[1], r[2] + g[2] + b[2]};
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(sum[i] - wtpt[i]) > 0.0002f) {
      return fail(error, "ICC colorants do not sum to the media white point");
    }
    if (std::fabs(wtpt[i] - kD50[i]) > 0.003f) {
      return fail(error, "ICC media white point is not D50");
    }
  }

  float adapted[9];
  bradford_d65_to_d50(rgb_to_xyz_d65, adapted);
  const float expect[3][3] = {
      {adapted[0], adapted[3], adapted[6]},
      {adapted[1], adapted[4], adapted[7]},
      {adapted[2], adapted[5], adapted[8]},
  };
  const float* got[3] = {r, g, b};
  for (int c = 0; c < 3; ++c) {
    for (int i = 0; i < 3; ++i) {
      if (std::fabs(got[c][i] - expect[c][i]) > 0.0002f) {
        return fail(error, "ICC colorants do not match the adapted primaries");
      }
    }
  }

  uint32_t trc_off = 0;
  uint32_t trc_sz = 0;
  if (!find(0x72545243u, &trc_off, &trc_sz)) return fail(error, "ICC rTRC is missing");
  for (uint32_t sig : {0x67545243u, 0x62545243u}) {
    uint32_t o = 0;
    uint32_t n = 0;
    if (!find(sig, &o, &n) || o != trc_off || n != trc_sz) {
      return fail(error, "ICC TRC tags do not share one curve");
    }
  }
  if (trc_sz < 12 || std::memcmp(icc.data() + trc_off, "curv", 4) != 0) {
    return fail(error, "ICC TRC is not a curv tag");
  }
  const uint32_t count = read_be32(icc.data() + trc_off + 8);
  if (linear) {
    if (count != 0) return fail(error, "linear ICC TRC must be a curv of count 0");
  } else {
    if (count != 1024 || trc_sz < 12u + 1024u * 2u) return fail(error, "Display P3 TRC must have 1024 entries");
    auto entry = [&](int i) {
      return read_be16(icc.data() + trc_off + 12 + static_cast<uint32_t>(i) * 2u);
    };
    auto expect_e = [](int i) {
      const float y = std::clamp(srgb_eotf(static_cast<float>(i) / 1023.0f), 0.0f, 1.0f);
      return static_cast<int>(std::lround(y * 65535.0f));
    };
    if (entry(0) > 1 || std::abs(static_cast<int>(entry(1023)) - 65535) > 1 ||
        std::abs(static_cast<int>(entry(512)) - expect_e(512)) > 2) {
      return fail(error, "Display P3 TRC is not the sRGB curve");
    }
  }
  return true;
}

struct TagRef {
  const uint8_t* p = nullptr;
  uint32_t n = 0;
};

TagRef find_tag(const std::vector<uint8_t>& icc, uint32_t sig) {
  if (icc.size() < 132) return {};
  const uint32_t ntags = read_be32(icc.data() + 128);
  if (ntags > (icc.size() - 132) / 12) return {};
  for (uint32_t i = 0; i < ntags; ++i) {
    const uint8_t* e = icc.data() + 132 + static_cast<size_t>(i) * 12u;
    if (read_be32(e) != sig) continue;
    const uint32_t off = read_be32(e + 4);
    const uint32_t n = read_be32(e + 8);
    if (off > icc.size() || n > icc.size() - off) return {};
    return {icc.data() + off, n};
  }
  return {};
}

bool colorant(const std::vector<uint8_t>& icc, uint32_t sig, float xyz[3]) {
  const TagRef t = find_tag(icc, sig);
  if (!t.p || t.n < 20 || std::memcmp(t.p, "XYZ ", 4) != 0) return false;
  for (int i = 0; i < 3; ++i) xyz[i] = read_s15(t.p + 8 + 4 * i);
  return true;
}

double para_eval(uint16_t type, const double* v, double x) {
  const double g = v[0], a = v[1], b = v[2], c = v[3], d = v[4], e = v[5], f = v[6];
  auto pw = [g](double base) { return base > 0.0 ? std::pow(base, g) : 0.0; };
  switch (type) {
    case 0: return pw(x);
    case 1: return (a != 0.0 && x >= -b / a) ? pw(a * x + b) : 0.0;
    case 2: return (a != 0.0 && x >= -b / a) ? pw(a * x + b) + c : c;
    case 3: return x >= d ? pw(a * x + b) : c * x;
    default: return x >= d ? pw(a * x + b) + e : c * x + f;
  }
}

template <typename Curve>
IccTransfer curve_verdict(Curve curve) {
  bool srgb = true;
  bool linear = true;
  for (int k = 0; k <= 16; ++k) {
    const double x = k / 16.0;
    const double y = curve(x);
    if (std::fabs(y - srgb_eotf(static_cast<float>(x))) > 0.002) srgb = false;
    if (std::fabs(y - x) > 0.002) linear = false;
  }
  if (srgb) return IccTransfer::kSrgb;
  if (linear) return IccTransfer::kLinear;
  return IccTransfer::kUnknown;
}

IccTransfer classify_trc(const TagRef& t) {
  if (!t.p || t.n < 12) return IccTransfer::kUnknown;
  if (std::memcmp(t.p, "curv", 4) == 0) {
    const uint32_t count = read_be32(t.p + 8);
    if (count > (t.n - 12) / 2) return IccTransfer::kUnknown;
    if (count == 0) return IccTransfer::kLinear;
    if (count == 1) {
      const double gamma = read_be16(t.p + 12) / 256.0;
      return curve_verdict([gamma](double x) { return std::pow(x, gamma); });
    }
    return curve_verdict([&](double x) {
      const double pos = x * (count - 1);
      const uint32_t i0 = std::min(static_cast<uint32_t>(pos), count - 1);
      const uint32_t i1 = std::min(i0 + 1, count - 1);
      const double y0 = read_be16(t.p + 12 + 2 * i0) / 65535.0;
      const double y1 = read_be16(t.p + 12 + 2 * i1) / 65535.0;
      return y0 + (y1 - y0) * (pos - i0);
    });
  }
  if (std::memcmp(t.p, "para", 4) == 0) {
    static const int kParams[5] = {1, 3, 4, 5, 7};
    const uint16_t type = read_be16(t.p + 8);
    if (type > 4 || t.n < 12u + 4u * static_cast<uint32_t>(kParams[type])) return IccTransfer::kUnknown;
    double v[7] = {1, 1, 0, 0, 0, 0, 0};
    for (int i = 0; i < kParams[type]; ++i) v[i] = read_s15(t.p + 12 + 4 * i);
    return curve_verdict([type, &v](double x) { return para_eval(type, v, x); });
  }
  return IccTransfer::kUnknown;
}

bool fail_read(std::string* error, const std::string& msg) {
  if (error) *error = msg;
  return false;
}

uint16_t tiff16(const uint8_t* p, bool le) {
  return le ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : read_be16(p);
}

uint32_t tiff32(const uint8_t* p, bool le) {
  return le ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)
            : read_be32(p);
}

bool icc_from_tiff(const std::vector<uint8_t>& file, std::vector<uint8_t>* icc, std::string* error) {
  const bool le = file[0] == 'I';
  const uint32_t ifd = tiff32(file.data() + 4, le);
  if (static_cast<size_t>(ifd) + 2 > file.size()) return fail_read(error, "TIFF IFD is past the end of the file");
  const uint16_t ntags = tiff16(file.data() + ifd, le);
  for (uint16_t i = 0; i < ntags; ++i) {
    const size_t at = ifd + 2u + static_cast<size_t>(i) * 12u;
    if (at + 12 > file.size()) return fail_read(error, "TIFF IFD entry is past the end of the file");
    const uint8_t* e = file.data() + at;
    if (tiff16(e, le) != 34675) continue;
    const uint32_t count = tiff32(e + 4, le);
    if (count < 128) return fail_read(error, "TIFF ICC tag is shorter than an ICC header");
    const uint32_t off = tiff32(e + 8, le);
    if (static_cast<size_t>(off) + count > file.size()) return fail_read(error, "TIFF ICC tag is past the end of the file");
    icc->assign(file.begin() + off, file.begin() + off + count);
    return true;
  }
  return fail_read(error, "TIFF has no ICC profile (tag 34675)");
}

bool icc_from_jpeg(const std::vector<uint8_t>& file, std::vector<uint8_t>* icc, std::string* error) {
  struct Chunk {
    int seq = 0;
    std::vector<uint8_t> data;
  };
  std::vector<Chunk> chunks;
  int total = -1;
  size_t i = 2;
  while (i + 1 < file.size()) {
    if (file[i] != 0xff) return fail_read(error, "JPEG marker was not FF");
    while (i < file.size() && file[i] == 0xff) ++i;
    if (i >= file.size()) break;
    const uint8_t marker = file[i++];
    if (marker == 0xd9 || marker == 0xda) break;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (i + 2 > file.size()) return fail_read(error, "JPEG marker length is truncated");
    const uint16_t seglen = read_be16(file.data() + i);
    if (seglen < 2 || i + seglen > file.size()) return fail_read(error, "JPEG marker is past the end of the file");
    if (marker == 0xe2 && seglen >= 16 && std::memcmp(file.data() + i + 2, "ICC_PROFILE", 12) == 0) {
      const uint8_t* p = file.data() + i + 2;
      const int seq = p[12];
      const int count = p[13];
      if (seq < 1 || count < 1 || seq > count) return fail_read(error, "JPEG ICC chunk sequence is invalid");
      if (total < 0) total = count;
      if (count != total) return fail_read(error, "JPEG ICC chunks disagree on the chunk count");
      chunks.push_back(Chunk{seq, std::vector<uint8_t>(p + 14, file.data() + i + seglen)});
    }
    i += seglen;
  }
  if (chunks.empty()) return fail_read(error, "JPEG has no ICC_PROFILE marker");
  if (static_cast<int>(chunks.size()) != total) {
    return fail_read(error, "JPEG has no complete ICC_PROFILE sequence");
  }
  std::sort(chunks.begin(), chunks.end(), [](const Chunk& a, const Chunk& b) { return a.seq < b.seq; });
  icc->clear();
  for (int seq = 1; seq <= total; ++seq) {
    const Chunk& c = chunks[static_cast<size_t>(seq - 1)];
    if (c.seq != seq) return fail_read(error, "JPEG ICC chunk sequence has a gap");
    icc->insert(icc->end(), c.data.begin(), c.data.end());
  }
  if (icc->size() < 128) return fail_read(error, "JPEG ICC profile is shorter than an ICC header");
  return true;
}

}  // namespace

std::vector<uint8_t> build_linear_rec2020_icc() { return build_profile("Linear Rec.2020", kRec2020ToXyzD65, true); }

std::vector<uint8_t> build_display_p3_icc() { return build_profile("Display P3", kDisplayP3ToXyzD65, false); }

bool validate_linear_rec2020_icc(const std::vector<uint8_t>& icc, std::string* error) {
  return validate_profile(icc, "Linear Rec.2020", true, kRec2020ToXyzD65, error);
}

bool validate_display_p3_icc(const std::vector<uint8_t>& icc, std::string* error) {
  return validate_profile(icc, "Display P3", false, kDisplayP3ToXyzD65, error);
}

IccClass classify_icc(const std::vector<uint8_t>& icc) {
  IccClass out;
  if (icc.size() < 132 || std::memcmp(icc.data() + 16, "RGB ", 4) != 0 ||
      std::memcmp(icc.data() + 20, "XYZ ", 4) != 0 || find_tag(icc, 0x41324230u).p) {
    return out;
  }
  float r[3];
  float g[3];
  float b[3];
  if (colorant(icc, 0x7258595Au, r) && colorant(icc, 0x6758595Au, g) && colorant(icc, 0x6258595Au, b)) {
    const struct {
      IccPrimaries id;
      const float* m;
    } cands[] = {{IccPrimaries::kRec2020, kRec2020ToXyzD65},
                 {IccPrimaries::kDisplayP3, kDisplayP3ToXyzD65},
                 {IccPrimaries::kSrgb, kSrgbToXyzD65}};
    const float* got[3] = {r, g, b};
    for (const auto& cand : cands) {
      float m[9];
      bradford_d65_to_d50(cand.m, m);
      float worst = 0.0f;
      for (int ch = 0; ch < 3; ++ch) {
        for (int row = 0; row < 3; ++row) worst = std::max(worst, std::fabs(got[ch][row] - m[row * 3 + ch]));
      }
      if (worst <= 0.005f) {
        out.primaries = cand.id;
        break;
      }
    }
  }
  const IccTransfer tr = classify_trc(find_tag(icc, 0x72545243u));
  if (tr == classify_trc(find_tag(icc, 0x67545243u)) && tr == classify_trc(find_tag(icc, 0x62545243u))) {
    out.transfer = tr;
  }
  return out;
}

std::string icc_class_name(const IccClass& c) {
  const char* p = c.primaries == IccPrimaries::kRec2020     ? "Rec.2020"
                  : c.primaries == IccPrimaries::kDisplayP3 ? "Display P3"
                  : c.primaries == IccPrimaries::kSrgb      ? "sRGB"
                                                            : "unknown primaries";
  const char* t = c.transfer == IccTransfer::kLinear ? "linear"
                  : c.transfer == IccTransfer::kSrgb ? "sRGB curve"
                                                     : "unknown curve";
  return std::string(p) + " / " + t;
}

bool read_embedded_icc(const std::string& path, std::vector<uint8_t>* icc, std::string* error) {
  if (!icc) return fail_read(error, "internal: null ICC output");
  std::ifstream in = open_input_binary(path);
  if (!in) return fail_read(error, "could not open " + path);
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (file.size() < 8) return fail_read(error, "file is too small to hold a color profile: " + path);
  if ((file[0] == 'I' && file[1] == 'I') || (file[0] == 'M' && file[1] == 'M')) return icc_from_tiff(file, icc, error);
  if (file[0] == 0xff && file[1] == 0xd8) return icc_from_jpeg(file, icc, error);
  return fail_read(error, "not a TIFF or JPEG: " + path);
}

}  // namespace uhdr_repack
