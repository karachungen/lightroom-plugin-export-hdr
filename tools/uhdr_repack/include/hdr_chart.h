#pragma once

#include <string>

namespace uhdr_repack {

/** Write hdr-chart.tif, sdr-chart.jpg, and chart-manifest.json into dir. */
int write_hdr_chart_main(const std::string& dir);

/** Encode the chart with Color map defaults and print stop/color recovery. */
int check_hdr_chart_main(const std::string& dir);

}  // namespace uhdr_repack
