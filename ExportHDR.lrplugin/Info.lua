--[[----------------------------------------------------------------------------
  Ultra HDR Export — Lightroom Classic export destination (macOS + Windows + uhdr_repack CLI).
  https://github.com/karachungen/lightroom-plugin-export-hdr
----------------------------------------------------------------------------]]

return {
	LrSdkVersion = 14.0,
	LrSdkMinimumVersion = 14.0,
	LrToolkitIdentifier = "com.karachungen.lightroom.export.ultrahdr",
	LrPluginName = "Ultra HDR Export",
	VERSION = { major = 3, minor = 0, revision = 0, build = 0 },

	LrExportServiceProvider = {
		title = "ULTRA HDR",
		file = "ExportHDRFilterProvider.lua",
	},

	LrPluginInfoUrl = "https://github.com/karachungen/lightroom-plugin-export-hdr",
}
