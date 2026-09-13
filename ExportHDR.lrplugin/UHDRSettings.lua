--[[----------------------------------------------------------------------------
  HDR TIFF pass merge for dummy export (SDR JPEG + internal HDR TIFF).
  All encoder settings live in Ultra HDR (uhdr_repack --edit).
----------------------------------------------------------------------------]]

local UHDR = {}

UHDR.KEY = {
	keepIntermediates = "UHDR_keepIntermediates",
}

function UHDR.defaults()
	return {
		[UHDR.KEY.keepIntermediates] = false,
	}
end

local function copyTable(t)
	local o = {}
	if not t then
		return o
	end
	for k, v in pairs(t) do
		o[k] = v
	end
	return o
end

function UHDR.flattenExportSettings(propertyTable)
	local out = {}
	if not propertyTable then
		return out
	end
	local nested = propertyTable["< contents >"]
	if type(nested) == "table" then
		for k, v in pairs(nested) do
			out[k] = v
		end
	end
	for k, v in pairs(propertyTable) do
		if k ~= "< contents >" then
			out[k] = v
		end
	end
	return out
end

local function shouldCopyKeyForHdrAuxExport(key)
	if type(key) ~= "string" then
		return false
	end
	if key == "LR_minimizeEmbeddedMetadata" or key == "LR_export_removeMetadata" then
		return true
	end
	if string.match(key, "^LR_size_") then
		return true
	end
	if string.match(key, "^LR_resize") then
		return true
	end
	if string.match(key, "^LR_export_resize") then
		return true
	end
	if string.match(key, "^LR_export_dimensions") then
		return true
	end
	if string.match(key, "^LR_export_constraints") then
		return true
	end
	if string.match(key, "^LR_export_watermark") then
		return true
	end
	if string.match(key, "^LR_export_metadata") then
		return true
	end
	if string.match(key, "^LR_export_keyword") then
		return true
	end
	if string.match(key, "^LR_outputSharpening") then
		return true
	end
	return false
end

function UHDR.mergeHdrTiffSettings(baseExportSettings, tempDir)
	local flat = UHDR.flattenExportSettings(baseExportSettings)
	local s = {}
	for k, v in pairs(flat) do
		if shouldCopyKeyForHdrAuxExport(k) then
			s[k] = v
		end
	end

	s.LR_export_destinationType = "tempFolder"
	s.LR_export_destinationPathPrefix = tempDir
	s.LR_export_useSubfolder = false
	s.LR_export_subfolderName = ""
	UHDR.forceOverwriteExistingFiles(s)

	s.LR_exportServiceProvider = "com.adobe.ag.export.file"
	s.LR_reimportExportedPhoto = false

	s.LR_renamingTokensOn = true
	s.LR_extensionCase = "lowercase"
	s.LR_tokens = "{{image_name}}"

	s.LR_format = "TIFF"
	s.LR_tiff_compressionMethod = "compressionMethod_ZIP"
	s.LR_export_colorSpace = "Rec2020_hdr"
	s.LR_export_bitDepth = 32
	s.LR_enableHDRDisplay = true
	s.LR_maximumCompatibility = false

	return s
end

function UHDR.validate(_propertyTable)
	return nil
end

--- Skip Lightroom's Existing Files prompt and replace files at the export path.
function UHDR.forceOverwriteExistingFiles(settings)
	if not settings then
		return
	end
	settings.LR_collisionHandling = "overwrite"
end

function UHDR.applyDefaults(propertyTable)
	local d = UHDR.defaults()
	for k, v in pairs(d) do
		if propertyTable[k] == nil then
			propertyTable[k] = v
		end
	end
end

return UHDR
