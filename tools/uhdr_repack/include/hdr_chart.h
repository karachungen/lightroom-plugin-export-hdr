#pragma once

#include <string>

namespace uhdr_repack {

/** Write hdr-chart.tif, sdr-chart.jpg, and chart-manifest.json into dir. */
int write_hdr_chart_main(const std::string& dir);

/** Encode the chart with Color map defaults and grade it against the manifest. */
int check_hdr_chart_main(const std::string& dir);

/**
 * Grade an already exported Ultra HDR JPEG or HDR TIFF against the chart manifest.
 * manifest_path empty uses chart-manifest.json beside the file.
 */
int check_hdr_chart_file_main(const std::string& path, const std::string& manifest_path);

}  // namespace uhdr_repack
