#pragma once

#include "uhdr_encode.h"

#include <nlohmann/json.hpp>

#include <string>

namespace uhdr_repack {

/** Color map card: the session default. */
EncodeOptions color_map_preset();

/** Mono map card. */
EncodeOptions mono_map_preset();

/** Copy the delivery fields of "color" or "mono" into opt. Returns false for other names. */
bool apply_encode_preset(const std::string& name, EncodeOptions* opt);

/** Delivery fields with the web UI's camelCase keys. */
nlohmann::json encode_options_ui_json(const EncodeOptions& opt);

}  // namespace uhdr_repack
