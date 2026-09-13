#include "hdr_bridge.h"

#include "path_io.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace uhdr_repack {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

bool id_char_ok(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
}

}  // namespace

std::string hdr_bridge_sanitize_id(const std::string& id) {
  std::string out;
  out.reserve(id.size());
  for (char c : id) {
    out.push_back(id_char_ok(c) ? c : '_');
  }
  if (out.empty()) out = "item";
  return out;
}

std::string hdr_bridge_json_path(const std::string& work_dir) {
  return (path_from_utf8(work_dir) / "bridge.json").u8string();
}

std::string hdr_requests_dir(const std::string& work_dir) {
  return (path_from_utf8(work_dir) / "hdr_requests").u8string();
}

std::string hdr_request_path(const std::string& work_dir, const std::string& id) {
  return (path_from_utf8(hdr_requests_dir(work_dir)) / (hdr_bridge_sanitize_id(id) + ".req"))
      .u8string();
}

std::string hdr_done_path(const std::string& work_dir, const std::string& id) {
  return (path_from_utf8(hdr_requests_dir(work_dir)) / (hdr_bridge_sanitize_id(id) + ".done"))
      .u8string();
}

std::string hdr_priority_path(const std::string& work_dir) {
  return (path_from_utf8(hdr_requests_dir(work_dir)) / "priority.txt").u8string();
}

bool write_hdr_priority(const std::string& work_dir, const std::string& id, std::string* error) {
  return atomic_write_text_file(hdr_priority_path(work_dir), id + "\n", error);
}

bool atomic_write_text_file(const std::string& path, const std::string& body, std::string* error) {
  const fs::path dest = path_from_utf8(path);
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  if (ec) {
    if (error) *error = "Could not create directory: " + dest.parent_path().u8string();
    return false;
  }
  fs::path tmp = dest;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "Could not write: " + tmp.u8string();
      return false;
    }
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!out) {
      if (error) *error = "Could not write: " + tmp.u8string();
      return false;
    }
  }
  fs::rename(tmp, dest, ec);
  if (ec) {
    fs::remove(dest, ec);
    fs::rename(tmp, dest, ec);
  }
  if (ec) {
    if (error) *error = "Could not replace: " + dest.u8string();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool write_hdr_bridge_enabled(const std::string& work_dir, std::string* error) {
  return atomic_write_text_file(hdr_bridge_json_path(work_dir), "{\n  \"enabled\": true\n}\n", error);
}

bool hdr_bridge_enabled(const std::string& work_dir) {
  std::ifstream in(path_from_utf8(hdr_bridge_json_path(work_dir)));
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  try {
    const json root = json::parse(ss.str());
    return root.value("enabled", false);
  } catch (...) {
    return false;
  }
}

bool write_hdr_request(const std::string& work_dir, const std::string& id, std::string* error) {
  json root;
  root["id"] = id;
  return atomic_write_text_file(hdr_request_path(work_dir, id), root.dump(2) + "\n", error);
}

bool write_hdr_done(const std::string& work_dir, const std::string& id, const HdrRequestResult& result,
                    std::string* error) {
  json root;
  root["ok"] = result.ok;
  root["hdr_tiff"] = result.hdr_tiff;
  if (!result.error.empty()) root["error"] = result.error;
  return atomic_write_text_file(hdr_done_path(work_dir, id), root.dump(2) + "\n", error);
}

bool read_hdr_done(const std::string& work_dir, const std::string& id, HdrRequestResult* result,
                   std::string* error) {
  if (!result) {
    if (error) *error = "null result";
    return false;
  }
  std::ifstream in(path_from_utf8(hdr_done_path(work_dir, id)));
  if (!in) {
    if (error) *error = "missing done file";
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  try {
    const json root = json::parse(ss.str());
    result->ok = root.value("ok", false);
    result->hdr_tiff = root.value("hdr_tiff", "");
    result->error = root.value("error", "");
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

bool hdr_done_has_valid_tiff(const std::string& work_dir, const std::string& id) {
  HdrRequestResult result;
  if (!read_hdr_done(work_dir, id, &result, nullptr) || !result.ok || result.hdr_tiff.empty()) {
    return false;
  }
  std::error_code ec;
  return fs::exists(path_from_utf8(result.hdr_tiff), ec);
}

}  // namespace uhdr_repack
