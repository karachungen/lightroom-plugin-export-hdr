#pragma once

#include <string>

namespace uhdr_repack {

/** Instagram re-encodes both the base and the gain map at this libjpeg quality, chroma 4:2:0. */
constexpr int kInstagramJpegQuality = 61;

/**
 * Re-encode an Ultra HDR JPEG the way Instagram does: base and gain map through libjpeg at
 * kInstagramJpegQuality with 4:2:0 chroma, same sizes, original gain-map metadata.
 */
bool simulate_instagram(const std::string& in_path, const std::string& out_path, std::string* error);

/** Sibling path with "-instagram" before the extension. */
std::string instagram_output_path(const std::string& jpg_path);

/** uhdr_repack --simulate-instagram <in.jpg> <out.jpg> */
int cli_simulate_instagram_main(int argc, char** argv);

}  // namespace uhdr_repack
