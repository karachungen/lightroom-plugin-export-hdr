#pragma once

#include <string>
#include <vector>

namespace uhdr_repack {

struct HdrRequestResult {
  bool ok = false;
  std::string hdr_tiff;
  std::string error;
};

std::string hdr_bridge_sanitize_id(const std::string& id);
std::string hdr_bridge_json_path(const std::string& work_dir);
std::string hdr_requests_dir(const std::string& work_dir);
std::string hdr_request_path(const std::string& work_dir, const std::string& id);
std::string hdr_done_path(const std::string& work_dir, const std::string& id);
std::string hdr_priority_path(const std::string& work_dir);
bool write_hdr_priority(const std::string& work_dir, const std::string& id, std::string* error);

bool atomic_write_text_file(const std::string& path, const std::string& body, std::string* error);

bool write_hdr_bridge_enabled(const std::string& work_dir, std::string* error);
bool hdr_bridge_enabled(const std::string& work_dir);

bool write_hdr_request(const std::string& work_dir, const std::string& id, std::string* error);
bool write_hdr_done(const std::string& work_dir, const std::string& id, const HdrRequestResult& result,
                    std::string* error);
bool read_hdr_done(const std::string& work_dir, const std::string& id, HdrRequestResult* result,
                   std::string* error);
bool hdr_done_has_valid_tiff(const std::string& work_dir, const std::string& id);

}  // namespace uhdr_repack
