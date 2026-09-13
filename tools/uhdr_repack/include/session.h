#pragma once

#include "encode_engine.h"

#include <string>
#include <vector>

namespace uhdr_repack {

struct SessionItem {
  std::string id;
  std::string label;
  std::string sdr;
  std::string hdr_tiff;
  std::string out;
  std::string final_out;
  EncodeOptions encode_options;
  bool has_encode_override = false;
  bool skipped = false;
  std::string gainmap_in;
  std::string watermark_config;
  std::string metadata_patch;
  SliceAspect slice_aspect = SliceAspect::kNone;
  bool has_slice_override = false;
  float crop_offset = 0.5f;
  unsigned slice_count = 1;
  unsigned preview_slice_index = 1;
  unsigned output_width = 0;
  unsigned output_height = 0;
};

struct PreviewSession {
  int version = 1;
  std::string work_dir;
  std::string result_path;
  std::string dest_dir;
  std::string log_path;
  EncodeOptions default_encode_options;
  SliceAspect default_slice_aspect = SliceAspect::kNone;
  std::vector<SessionItem> items;
};

struct ItemEncodeResult {
  std::string id;
  bool skipped = false;
  bool encoded = false;
  std::string out;
  std::vector<std::string> slices;
  int exit_code = 0;
  std::string error;
};

struct PreviewResult {
  bool approved = false;
  std::string dest_dir;
  std::vector<ItemEncodeResult> items;
};

bool load_session_file(const std::string& path, PreviewSession* out, std::string* error);
bool write_result_file(const std::string& path, const PreviewResult& result, std::string* error);

EncodeOptions effective_encode_options(const PreviewSession& session, const SessionItem& item);

SliceAspect effective_slice_aspect(const PreviewSession& session, const SessionItem& item);

float effective_crop_offset(const SessionItem& item);

unsigned effective_slice_count(const SessionItem& item);

/** List numbered slice JPEG paths next to out_path after encode. */
std::vector<std::string> list_slice_output_paths(const std::string& out_path, SliceAspect aspect);

/** Parse CLI groups: repeated --sdr/--hdr-tiff/--out into session items. */
bool parse_edit_cli_args(int argc, char** argv, PreviewSession* session, std::string* error);

std::string resolve_repo_relative(const std::string& path, const std::string& base_dir);

int apply_session_batch(PreviewSession* session, PreviewResult* result, std::string* error);

/** Fill dest_dir if empty, then retarget each item.out to dest_dir / original filename. */
bool apply_session_dest_dir(PreviewSession* session, std::string* error);

std::string inferred_dest_dir(const PreviewSession& session);

}  // namespace uhdr_repack
