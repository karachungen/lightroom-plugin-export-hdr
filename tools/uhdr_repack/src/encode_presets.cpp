#include "encode_presets.h"

namespace uhdr_repack {

EncodeOptions color_map_preset() { return EncodeOptions{}; }

EncodeOptions mono_map_preset() {
  EncodeOptions o;
  o.gainmap_scale = 2;
  o.max_content_boost = 4.92f;
  o.target_display_peak_nits = 1000.0f;
  o.monochrome_gainmap = true;
  return o;
}

bool apply_encode_preset(const std::string& name, EncodeOptions* opt) {
  EncodeOptions p;
  if (name == "color") p = color_map_preset();
  else if (name == "mono") p = mono_map_preset();
  else return false;
  opt->base_quality = p.base_quality;
  opt->gainmap_quality = p.gainmap_quality;
  opt->gainmap_scale = p.gainmap_scale;
  opt->min_content_boost = p.min_content_boost;
  opt->max_content_boost = p.max_content_boost;
  opt->target_display_peak_nits = p.target_display_peak_nits;
  opt->monochrome_gainmap = p.monochrome_gainmap;
  return true;
}

nlohmann::json encode_options_ui_json(const EncodeOptions& o) {
  return {{"baseQuality", o.base_quality},
          {"gainmapQuality", o.gainmap_quality},
          {"gainmapScale", o.gainmap_scale},
          {"minBoost", o.min_content_boost},
          {"maxBoost", o.max_content_boost},
          {"displayPeak", o.target_display_peak_nits},
          {"monochromeGainmap", o.monochrome_gainmap}};
}

}  // namespace uhdr_repack
