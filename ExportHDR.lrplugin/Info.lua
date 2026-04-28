--[[----------------------------------------------------------------------------
  Ultra HDR Export — Lightroom Classic export filter (macOS 26 (Tahoe), ARM64 + uhdr_repack CLI).
  https://github.com/karachungen/lightroom-plugin-export-hdr
----------------------------------------------------------------------------]]

return {
	LrSdkVersion = 14.0,
	LrSdkMinimumVersion = 14.0,
	LrToolkitIdentifier = "com.karachungen.lightroom.export.ultrahdr",
	LrPluginName = "Ultra HDR Export",
	VERSION = { major = 1, minor = 0, revision = 0, build = 0 },

	LrExportFilterProvider = {
		{
			id = "ultra_hdr_uhdr_repack",
			title = "Encode Ultra HDR JPEG (uhdr_repack)",
			file = "ExportHDRFilterProvider.lua",
		},
	},

	LrPluginInfoUrl = "https://github.com/karachungen/lightroom-plugin-export-hdr",
}
