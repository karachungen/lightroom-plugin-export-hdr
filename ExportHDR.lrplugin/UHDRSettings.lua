--[[----------------------------------------------------------------------------
  Defaults, export-setting merge for HDR TIFF pass, validation.
  Named UHDRSettings.lua because import "Settings" conflicts with Lightroom namespaces.
----------------------------------------------------------------------------]]

local UHDR = {}

-- Plugin-specific keys (persist in export presets)
UHDR.KEY = {
	baseQuality = "UHDR_baseQuality",
	gainmapQuality = "UHDR_gainmapQuality",
	gainmapScale = "UHDR_gainmapScale",
	minContentBoost = "UHDR_minContentBoost",
	maxContentBoost = "UHDR_maxContentBoost",
	targetDisplayPeak = "UHDR_targetDisplayPeak",
	monochromeGainmap = "UHDR_monochromeGainmap",
	keepIntermediates = "UHDR_keepIntermediates",
	runInspect = "UHDR_runInspect",
	debugSaveArtifacts = "UHDR_debugSaveArtifacts",
	sliceAspect = "UHDR_sliceAspect",
}

function UHDR.defaults()
	return {
		[UHDR.KEY.baseQuality] = 92,
		[UHDR.KEY.gainmapQuality] = 85,
		[UHDR.KEY.gainmapScale] = 1,
		[UHDR.KEY.minContentBoost] = 1.0,
		[UHDR.KEY.maxContentBoost] = 1000.0,
		[UHDR.KEY.targetDisplayPeak] = 1000.0,
		[UHDR.KEY.monochromeGainmap] = false,
		[UHDR.KEY.keepIntermediates] = false,
		[UHDR.KEY.runInspect] = false,
		[UHDR.KEY.debugSaveArtifacts] = false,
		[UHDR.KEY.sliceAspect] = "none",
	}
end

function UHDR.sliceAspectEnabled(propertyTable)
	local v = propertyTable and propertyTable[UHDR.KEY.sliceAspect]
	return v == "1x1" or v == "4x5"
end

--- Shallow copy of a table (export settings are typically flat).
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

--[[
  Flatten SDK property table: built-in export keys may live under ["< contents >"].
  Shallow copy only — same as community export plug-in patterns.
]]
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

--[[
  Keys safe to copy from the user's export preset into the auxiliary HDR TIFF job.
  Full flatten+copy inherited JPEG/format/service keys and Lightroom often kept emitting
  JPEG for the second session despite LR_format = TIFF — whitelist avoids that bleed-through.
]]
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

--[[
  Build export settings for the auxiliary HDR TIFF render.
  Image sizing / sharpening / metadata come from the whitelist above; format + HDR + destination are fixed.
]]
function UHDR.mergeHdrTiffSettings(baseExportSettings, tempDir)
	local flat = UHDR.flattenExportSettings(baseExportSettings)
	local s = {}
	for k, v in pairs(flat) do
		if shouldCopyKeyForHdrAuxExport(k) then
			s[k] = v
		end
	end

	-- tempFolder matches Adobe HDR TIFF SDK samples; specificFolder still picked up stray JPEG pipeline from presets.
	s.LR_export_destinationType = "tempFolder"
	s.LR_export_destinationPathPrefix = tempDir
	s.LR_export_useSubfolder = false
	s.LR_export_subfolderName = ""
	s.LR_collisionHandling = "overwrite"

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

function UHDR.validate(propertyTable)
	local function bad(msg)
		return msg
	end

	local q = tonumber(propertyTable[UHDR.KEY.baseQuality])
	if not q or q < 0 or q > 100 then
		return bad("Base quality must be between 0 and 100.")
	end
	q = tonumber(propertyTable[UHDR.KEY.gainmapQuality])
	if not q or q < 0 or q > 100 then
		return bad("Gain map quality must be between 0 and 100.")
	end
	local gs = tonumber(propertyTable[UHDR.KEY.gainmapScale])
	if not gs or gs < 1 or gs > 16 then
		return bad("Gain map scale must be between 1 and 16.")
	end
	local mn = tonumber(propertyTable[UHDR.KEY.minContentBoost])
	local mx = tonumber(propertyTable[UHDR.KEY.maxContentBoost])
	if not mn or not mx or mn <= 0 or mx <= 0 or mn > mx then
		return bad("Content boost min/max must be positive and min <= max.")
	end
	local peak = tonumber(propertyTable[UHDR.KEY.targetDisplayPeak])
	if not peak or peak <= 0 or peak > 10000 then
		return bad("Target display peak must be a sensible nit value (1-10000).")
	end

	local sa = propertyTable[UHDR.KEY.sliceAspect]
	if sa ~= nil and sa ~= "none" and sa ~= "1x1" and sa ~= "4x5" then
		return bad("Slice aspect must be Off, 1:1, or 4:5.")
	end

	return nil
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
