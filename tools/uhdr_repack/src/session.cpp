#include "session.h"

#include "encode_engine.h"
#include "slice_plan.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace uhdr_repack {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string read_text_file(const std::string& path, std::string* error) {
  std::ifstream ifs(path);
  if (!ifs) {
    if (error) *error = "Could not open: " + path;
    return {};
  }
  std::ostringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

void parse_encode_options_json(const json& j, EncodeOptions* opt) {
  if (!opt || !j.is_object()) {
    return;
  }
  if (j.contains("base_quality")) opt->base_quality = j["base_quality"].get<int>();
  if (j.contains("gainmap_quality")) opt->gainmap_quality = j["gainmap_quality"].get<int>();
  if (j.contains("gainmap_scale")) opt->gainmap_scale = j["gainmap_scale"].get<int>();
  if (j.contains("min_content_boost")) opt->min_content_boost = j["min_content_boost"].get<float>();
  if (j.contains("max_content_boost")) opt->max_content_boost = j["max_content_boost"].get<float>();
  if (j.contains("target_display_peak_nits")) {
    opt->target_display_peak_nits = j["target_display_peak_nits"].get<float>();
  }
  if (j.contains("monochrome_gainmap")) opt->monochrome_gainmap = j["monochrome_gainmap"].get<bool>();
}

void parse_slice_aspect_json(const json& j, SliceAspect* out) {
  if (!out || !j.is_object() || !j.contains("slice_aspect")) {
    return;
  }
  parse_slice_aspect(j["slice_aspect"].get<std::string>(), out);
}

SessionItem parse_item(const json& j) {
  SessionItem item;
  item.id = j.value("id", "");
  item.label = j.value("label", item.id);
  item.sdr = j.value("sdr", "");
  item.hdr_tiff = j.value("hdr_tiff", "");
  item.out = j.value("out", "");
  item.final_out = j.value("final_out", "");
  item.skipped = j.value("skipped", false);
  item.gainmap_in = j.value("gainmap_in", "");
  item.watermark_config = j.value("watermark_config", "");
  item.metadata_patch = j.value("metadata_patch", "");
  if (j.contains("encode")) {
    item.has_encode_override = true;
    parse_encode_options_json(j["encode"], &item.encode_options);
    parse_slice_aspect_json(j["encode"], &item.slice_aspect);
    if (j["encode"].contains("slice_aspect")) {
      item.has_slice_override = true;
    }
    if (j["encode"].contains("slice_count") && j["encode"]["slice_count"].is_number()) {
      const int n = j["encode"]["slice_count"].get<int>();
      item.slice_count = n < 0 ? 0u : static_cast<unsigned>(n);
    }
    if (j["encode"].contains("output_width") && j["encode"]["output_width"].is_number()) {
      item.output_width = static_cast<unsigned>(std::max(0, j["encode"]["output_width"].get<int>()));
    }
    if (j["encode"].contains("output_height") && j["encode"]["output_height"].is_number()) {
      item.output_height = static_cast<unsigned>(std::max(0, j["encode"]["output_height"].get<int>()));
    }
  }
  if (j.contains("slice_aspect")) {
    item.has_slice_override = true;
    parse_slice_aspect(j.value("slice_aspect", ""), &item.slice_aspect);
  }
  if (j.contains("instagram_preset")) {
    item.has_slice_override = true;
    parse_slice_aspect(j.value("instagram_preset", ""), &item.slice_aspect);
  }
  if (j.contains("crop_offset") && j["crop_offset"].is_number()) {
    item.crop_offset = std::clamp(j["crop_offset"].get<float>(), 0.0f, 1.0f);
  }
  if (j.contains("slice_count") && j["slice_count"].is_number()) {
    const int n = j["slice_count"].get<int>();
    item.slice_count = n < 0 ? 0u : static_cast<unsigned>(n);
  }
  if (j.contains("preview_slice_index") && j["preview_slice_index"].is_number()) {
    const int idx = j["preview_slice_index"].get<int>();
    item.preview_slice_index = idx < 1 ? 1u : static_cast<unsigned>(idx);
  }
  if (j.contains("output_width") && j["output_width"].is_number()) {
    item.output_width = static_cast<unsigned>(std::max(0, j["output_width"].get<int>()));
  }
  if (j.contains("output_height") && j["output_height"].is_number()) {
    item.output_height = static_cast<unsigned>(std::max(0, j["output_height"].get<int>()));
  }
  return item;
}

}  // namespace

std::string resolve_repo_relative(const std::string& path, const std::string& base_dir) {
  if (path.empty()) {
    return path;
  }
  fs::path p(path);
  if (p.is_absolute()) {
    return fs::weakly_canonical(p).string();
  }
  fs::path base(base_dir.empty() ? fs::current_path() : fs::path(base_dir));
  return fs::weakly_canonical(base / p).string();
}

bool load_session_file(const std::string& path, PreviewSession* out, std::string* error) {
  if (!out) {
    if (error) *error = "null session output";
    return false;
  }

  std::string err;
  const std::string text = read_text_file(path, &err);
  if (text.empty() && !err.empty()) {
    if (error) *error = err;
    return false;
  }

  json root;
  try {
    root = json::parse(text);
  } catch (const std::exception& ex) {
    if (error) *error = std::string("JSON parse error: ") + ex.what();
    return false;
  }

  PreviewSession session;
  session.version = root.value("version", 1);
  session.work_dir = root.value("work_dir", "");
  session.result_path = root.value("result_path", "");
  session.dest_dir = root.value("dest_dir", "");
  session.log_path = root.value("log_path", "");

  if (root.contains("default_encode")) {
    parse_encode_options_json(root["default_encode"], &session.default_encode_options);
    parse_slice_aspect_json(root["default_encode"], &session.default_slice_aspect);
  }

  const std::string base_dir = fs::current_path().string();

  if (root.contains("items") && root["items"].is_array()) {
    for (const auto& item_j : root["items"]) {
      SessionItem item = parse_item(item_j);
      item.sdr = resolve_repo_relative(item.sdr, base_dir);
      item.hdr_tiff = resolve_repo_relative(item.hdr_tiff, base_dir);
      item.out = resolve_repo_relative(item.out, base_dir);
      if (!item.final_out.empty()) {
        item.final_out = resolve_repo_relative(item.final_out, base_dir);
      }
      if (!item.gainmap_in.empty()) {
        item.gainmap_in = resolve_repo_relative(item.gainmap_in, base_dir);
      }
      if (!item.watermark_config.empty()) {
        item.watermark_config = resolve_repo_relative(item.watermark_config, base_dir);
      }
      if (!item.metadata_patch.empty()) {
        item.metadata_patch = resolve_repo_relative(item.metadata_patch, base_dir);
      }
      session.items.push_back(std::move(item));
    }
  }

  if (!session.work_dir.empty()) {
    session.work_dir = resolve_repo_relative(session.work_dir, base_dir);
  }
  if (!session.result_path.empty()) {
    session.result_path = resolve_repo_relative(session.result_path, base_dir);
  }
  if (!session.dest_dir.empty()) {
    session.dest_dir = resolve_repo_relative(session.dest_dir, base_dir);
  }
  if (!session.log_path.empty()) {
    session.log_path = resolve_repo_relative(session.log_path, base_dir);
  }
  if (session.dest_dir.empty()) {
    session.dest_dir = inferred_dest_dir(session);
  }

  *out = std::move(session);
  return true;
}

bool write_result_file(const std::string& path, const PreviewResult& result, std::string* error) {
  json root;
  root["approved"] = result.approved;
  if (!result.dest_dir.empty()) {
    root["dest_dir"] = result.dest_dir;
  }
  root["items"] = json::array();
  for (const auto& item : result.items) {
    json j;
    j["id"] = item.id;
    j["skipped"] = item.skipped;
    j["encoded"] = item.encoded;
    j["out"] = item.out;
    j["exit_code"] = item.exit_code;
    if (!item.slices.empty()) {
      j["slices"] = item.slices;
    }
    if (!item.error.empty()) {
      j["error"] = item.error;
    }
    root["items"].push_back(std::move(j));
  }

  fs::path p(path);
  if (p.has_parent_path()) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
  }

  std::ofstream ofs(path);
  if (!ofs) {
    if (error) *error = "Could not write result: " + path;
    return false;
  }
  ofs << root.dump(2) << '\n';
  return true;
}

EncodeOptions effective_encode_options(const PreviewSession& session, const SessionItem& item) {
  if (item.has_encode_override) {
    return item.encode_options;
  }
  return session.default_encode_options;
}

SliceAspect effective_slice_aspect(const PreviewSession& session, const SessionItem& item) {
  if (item.has_slice_override) {
    return item.slice_aspect;
  }
  return session.default_slice_aspect;
}

float effective_crop_offset(const SessionItem& item) {
  return std::clamp(item.crop_offset, 0.0f, 1.0f);
}

unsigned effective_slice_count(const SessionItem& item) {
  return item.slice_count;
}

std::vector<std::string> list_slice_output_paths(const std::string& out_path, SliceAspect aspect) {
  std::vector<std::string> paths;
  if (aspect == SliceAspect::kNone || out_path.empty()) {
    return paths;
  }
  const fs::path out = fs::u8path(out_path);
  const fs::path folder = out.has_parent_path() ? out.parent_path() : fs::current_path();
  const std::string stem = out.stem().u8string();
  const std::string ext = out.extension().u8string();
  const std::string label = slice_aspect_label(aspect);
  const std::string prefix = stem + "_" + label + "_";
  std::error_code ec;
  if (!fs::exists(folder, ec)) {
    return paths;
  }
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string leaf = entry.path().filename().u8string();
    if (leaf.size() <= prefix.size()) continue;
    if (leaf.compare(0, prefix.size(), prefix) != 0) continue;
    if (entry.path().extension().u8string() != ext) continue;
    paths.push_back(entry.path().u8string());
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

bool parse_edit_cli_args(int argc, char** argv, PreviewSession* session, std::string* error) {
  if (!session) {
    if (error) *error = "null session";
    return false;
  }

  SessionItem current;
  bool have_item = false;
  std::string session_path;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--session" && i + 1 < argc) {
      session_path = argv[++i];
    } else if (a == "--sdr" && i + 1 < argc) {
      if (have_item && !current.sdr.empty()) {
        session->items.push_back(current);
        current = SessionItem{};
      }
      current.sdr = argv[++i];
      have_item = true;
    } else if (a == "--hdr-tiff" && i + 1 < argc) {
      current.hdr_tiff = argv[++i];
      have_item = true;
    } else if (a == "--out" && i + 1 < argc) {
      current.out = argv[++i];
      have_item = true;
    } else if (a == "--gainmap-in" && i + 1 < argc) {
      current.gainmap_in = argv[++i];
    } else if (a == "--watermark-config" && i + 1 < argc) {
      current.watermark_config = argv[++i];
    } else if (a == "--metadata-patch" && i + 1 < argc) {
      current.metadata_patch = argv[++i];
    }
  }

  if (have_item && !current.sdr.empty()) {
    if (current.id.empty()) {
      current.id = "item_" + std::to_string(session->items.size() + 1);
    }
    if (current.label.empty()) {
      current.label = current.id;
    }
    session->items.push_back(current);
  }

  if (!session_path.empty()) {
    PreviewSession loaded;
    if (!load_session_file(session_path, &loaded, error)) {
      return false;
    }
    if (session->items.empty()) {
      *session = std::move(loaded);
    } else {
      for (auto& item : session->items) {
        item.sdr = resolve_repo_relative(item.sdr, fs::current_path().string());
        item.hdr_tiff = resolve_repo_relative(item.hdr_tiff, fs::current_path().string());
        item.out = resolve_repo_relative(item.out, fs::current_path().string());
      }
      if (session->work_dir.empty()) session->work_dir = loaded.work_dir;
      if (session->result_path.empty()) session->result_path = loaded.result_path;
      if (session->dest_dir.empty()) session->dest_dir = loaded.dest_dir;
      if (session->log_path.empty()) session->log_path = loaded.log_path;
      session->default_encode_options = loaded.default_encode_options;
    }
  }

  if (session->items.empty()) {
    if (error) *error = "No items in edit session (use --session or --sdr/--hdr-tiff/--out groups)";
    return false;
  }

  for (auto& item : session->items) {
    if (item.id.empty()) {
      item.id = "item_" + std::to_string(&item - &session->items[0] + 1);
    }
    if (item.label.empty()) {
      item.label = item.id;
    }
  }

  return true;
}

std::string inferred_dest_dir(const PreviewSession& session) {
  for (const auto& item : session.items) {
    const std::string named = item.final_out.empty() ? item.out : item.final_out;
    if (named.empty()) {
      continue;
    }
    const fs::path parent = fs::u8path(named).parent_path();
    if (!parent.empty()) {
      return parent.u8string();
    }
  }
  return {};
}

bool apply_session_dest_dir(PreviewSession* session, std::string* error) {
  if (!session) {
    if (error) *error = "null session";
    return false;
  }
  if (session->dest_dir.empty()) {
    session->dest_dir = inferred_dest_dir(*session);
  }
  if (session->dest_dir.empty()) {
    return true;
  }

  std::error_code ec;
  fs::create_directories(fs::u8path(session->dest_dir), ec);
  if (ec) {
    if (error) *error = "Could not create destination folder: " + session->dest_dir;
    return false;
  }

  const fs::path dest = fs::u8path(session->dest_dir);
  for (auto& item : session->items) {
    const std::string named = item.final_out.empty() ? item.out : item.final_out;
    fs::path leaf = fs::u8path(named).filename();
    if (leaf.empty()) {
      leaf = fs::u8path(item.id + ".jpg");
    }
    item.out = (dest / leaf).u8string();
  }
  return true;
}

int apply_session_batch(PreviewSession* session, PreviewResult* result, std::string* error) {
  if (!session || !result) {
    if (error) *error = "null session/result";
    return 1;
  }

  if (!apply_session_dest_dir(session, error)) {
    return 1;
  }

  result->approved = true;
  result->dest_dir = session->dest_dir;
  result->items.clear();

  for (const auto& item : session->items) {
    ItemEncodeResult ir;
    ir.id = item.id;
    ir.skipped = item.skipped;
    ir.out = item.out;

    if (item.skipped) {
      ir.encoded = false;
      result->items.push_back(ir);
      continue;
    }

    EncodeRequest req;
    req.hdr_tiff = item.hdr_tiff;
    req.base_path = item.sdr;
    req.out_path = item.out;
    req.options = effective_encode_options(*session, item);
    req.gainmap_in = item.gainmap_in;
    req.watermark_config = item.watermark_config;
    req.metadata_patch = item.metadata_patch;
    req.slice_aspect = effective_slice_aspect(*session, item);
    req.crop_offset = effective_crop_offset(item);
    req.slice_count = effective_slice_count(item);
    req.preview_slice_index = 0;
    req.output_width = item.output_width;
    req.output_height = item.output_height;

    std::string enc_err;
    ir.exit_code = encode_from_paths(req, &enc_err);
    ir.encoded = (ir.exit_code == 0);
    ir.error = enc_err;
    if (ir.encoded && req.slice_aspect != SliceAspect::kNone) {
      ir.slices = list_slice_output_paths(item.out, req.slice_aspect);
    }
    result->items.push_back(ir);

    if (ir.exit_code != 0) {
      result->approved = false;
    }
  }

  if (!session->result_path.empty()) {
    if (!write_result_file(session->result_path, *result, error)) {
      return 1;
    }
  }

  for (const auto& ir : result->items) {
    if (!ir.skipped && ir.exit_code != 0) {
      return ir.exit_code;
    }
  }
  return 0;
}

}  // namespace uhdr_repack
