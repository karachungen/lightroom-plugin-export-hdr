#pragma once

#include <string>

namespace uhdr_repack {

/** Write hdr-chart.tif, sdr-chart.jpg, and chart-manifest.json into dir. */
int write_hdr_chart_main(const std::string& dir);

/**
 * Encode the chart with encode_session_item (the editor's Apply) using encode options parsed like
 * the CLI (--preset, --slice-aspect, --out-width, ...), verify delivery, then grade at SDR,
 * partial, and full headroom.
 */
int check_hdr_chart_main(int argc, char** argv);

/**
 * Grade an exported Ultra HDR JPEG or HDR TIFF against the manifest. --slice-aspect names the
 * Instagram frame the export used. Only full headroom gates a JPEG; Lightroom renders its own SDR base.
 */
int check_hdr_chart_file_main(int argc, char** argv);

/** Fail unless hdr-chart.tif and chart-manifest.json in both directories match byte for byte. */
int check_hdr_chart_assets_main(const std::string& committed_dir, const std::string& fresh_dir);

}  // namespace uhdr_repack
