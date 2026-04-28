--[[----------------------------------------------------------------------------
  Ultra HDR Export — Lightroom Classic export filter (macOS + uhdr_repack CLI).
  https://github.com/karachungen/lightroom-plugin-export-hdr
----------------------------------------------------------------------------]]

return {
	LrSdkVersion = 13.0,
	LrSdkMinimumVersion = 13.0,
	LrToolkitIdentifier = "com.karachungen.lightroom.export.ultrahdr",
	LrPluginName = "Ultra HDR Export (uhdr_repack)",
	VERSION = { major = 1, minor = 0, revision = 0, build = 0 },

	LrExportFilterProvider = {
		{
			id = "ultra_hdr_uhdr_repack",
			title = "Ultra HDR (uhdr_repack)",
			file = "ExportHDRFilterProvider.lua",
		},
	},

	LrPluginInfoUrl = "https://github.com/karachungen/lightroom-plugin-export-hdr",
}
