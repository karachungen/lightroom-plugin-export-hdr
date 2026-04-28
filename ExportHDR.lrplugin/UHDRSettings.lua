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
	verboseLog = "UHDR_verboseLog",
	debugSaveArtifacts = "UHDR_debugSaveArtifacts",
}

function UHDR.defaults()
	return {
		[UHDR.KEY.baseQuality] = 92,
		[UHDR.KEY.gainmapQuality] = 85,
		[UHDR.KEY.gainmapScale] = 1,
		[UHDR.KEY.minContentBoost] = 1.0,
		[UHDR.KEY.maxContentBoost] = 100.0,
		[UHDR.KEY.targetDisplayPeak] = 1000.0,
		[UHDR.KEY.monochromeGainmap] = false,
		[UHDR.KEY.keepIntermediates] = false,
		[UHDR.KEY.runInspect] = false,
		[UHDR.KEY.verboseLog] = false,
		[UHDR.KEY.debugSaveArtifacts] = false,
	}
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
  Build export settings for the auxiliary HDR TIFF render.
  Keeps the user's Image Sizing and most options; overrides format/HDR/temp output.
  Keys follow community / preset inspection patterns for Lightroom 13+ HDR TIFF.
]]
function UHDR.mergeHdrTiffSettings(baseExportSettings, tempDir)
	local s = copyTable(UHDR.flattenExportSettings(baseExportSettings))

	s.LR_export_destinationType = "specificFolder"
	s.LR_export_destinationPathPrefix = tempDir
	s.LR_export_useSubfolder = false
	s.LR_collisionHandling = "overwrite"

	s.LR_renamingTokensOn = true
	s.LR_extensionCase = "lowercase"
	s.LR_tokens = "{{image_name}}"

	s.LR_format = "TIFF"
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
