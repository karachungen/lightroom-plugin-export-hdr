--[[----------------------------------------------------------------------------
  Export filter: renders user's normal export (SDR/base), renders matching HDR TIFF,
  runs uhdr_repack to replace the exported file with an Ultra HDR JPEG.
----------------------------------------------------------------------------]]

local LrView = import "LrView"
local LrDialogs = import "LrDialogs"
local LrTasks = import "LrTasks"
local LrProgressScope = import "LrProgressScope"
local LrPathUtils = import "LrPathUtils"
local LrFileUtils = import "LrFileUtils"
local LrExportSession = import "LrExportSession"

local loadPluginModule = assert(loadfile(LrPathUtils.child(_PLUGIN.path, "PluginInit.lua")))()
local UHDR = loadPluginModule("UHDRSettings")
local CMD = loadPluginModule("Command")
local Log = loadPluginModule("Log")

local ExportHDRFilterProvider = {}

--- Post-process picker, export dialog section, and progress (distinct from LrPluginName umbrella in Info.lua).
local FILTER_UI_TITLE = "Encode Ultra HDR JPEG (uhdr_repack)"

--- Build exportPresetFields from defaults (preset persistence).
local exportPresetFields = {}
do
	for k, v in pairs(UHDR.defaults()) do
		exportPresetFields[#exportPresetFields + 1] = { key = k, default = v }
	end
end
ExportHDRFilterProvider.exportPresetFields = exportPresetFields

function ExportHDRFilterProvider.startDialog(propertyTable)
	UHDR.applyDefaults(propertyTable)
end

function ExportHDRFilterProvider.sectionForFilterInDialog(f, propertyTable)
	UHDR.applyDefaults(propertyTable)
	local bind = LrView.bind
	local K = UHDR.KEY

	local LW = 20

	return {
		title = FILTER_UI_TITLE,
		synopsis = "Encodes Ultra HDR JPEG via uhdr_repack. Use JPEG in File Settings for the SDR base; the plug-in adds a separate HDR TIFF pass internally. Image Sizing applies to both passes.",
		f:row {
			fill_horizontal = 1,
			f:column {
				fill_horizontal = 1,
				spacing = f:label_spacing(),
				f:row {
					fill_horizontal = 1,
					f:static_text {
						fill_horizontal = 1,
						width_in_chars = 55,
						title = table.concat({
							"How to use:",
							"In Post-Process Actions, click Add → Ultra HDR Export → Encode Ultra HDR JPEG (uhdr_repack). If Lightroom shows Install, click it, then expand the action so this panel appears.",
							"Under File Settings, pick JPEG (recommended) so Lightroom renders a normal SDR base; the final file is replaced with an Ultra HDR .jpg.",
							"Do not use TIFF + HDR Output + 32-bit for the main export. The plug-in runs its own internal HDR TIFF pass.",
							"Use HDR editing in Develop when needed (Lightroom 14+).",
						}, "\n"),
						height_in_lines = 7,
					},
				},
				f:spacer { height = f:control_spacing() },
				f:row {
					f:static_text {
						title = "Base quality",
						width_in_chars = LW,
					},
					f:edit_field {
						value = bind { key = K.baseQuality, object = propertyTable },
						immediate = true,
						width = 80,
					},
					f:static_text { title = "(0-100)" },
				},
				f:row {
					f:static_text {
						title = "Gain map Q",
						width_in_chars = LW,
					},
					f:edit_field {
						value = bind { key = K.gainmapQuality, object = propertyTable },
						immediate = true,
						width = 80,
					},
				},
				f:row {
					f:static_text {
						title = "Gain map scale",
						width_in_chars = LW,
					},
					f:edit_field {
						value = bind { key = K.gainmapScale, object = propertyTable },
						immediate = true,
						width = 80,
						tooltip = "1 = gain map same pixel size as the base image; larger values use a smaller gain map (smaller file).",
					},
				},
				f:row {
					f:static_text {
						title = "Min / max boost",
						width_in_chars = LW,
					},
					f:edit_field {
						value = bind { key = K.minContentBoost, object = propertyTable },
						immediate = true,
						width = 64,
					},
					f:static_text { title = "-" },
					f:edit_field {
						value = bind { key = K.maxContentBoost, object = propertyTable },
						immediate = true,
						width = 64,
					},
				},
				f:row {
					f:static_text {
						title = "Display peak",
						width_in_chars = LW,
					},
					f:edit_field {
						value = bind { key = K.targetDisplayPeak, object = propertyTable },
						immediate = true,
						width = 80,
					},
					f:static_text { title = "nits" },
				},
				f:row {
					f:static_text {
						title = "Slicing",
						width_in_chars = LW,
					},
					f:popup_menu {
						value = bind { key = K.sliceAspect, object = propertyTable },
						width_in_chars = 12,
						items = {
							{ title = "Off", value = "none" },
							{ title = "1:1", value = "1x1" },
							{ title = "4:5", value = "4x5" },
						},
						tooltip = "Optional full-height slices at 1:1 or 4:5. Keeps the original Ultra HDR file and writes numbered slices next to it.",
					},
				},
				f:row {
					f:static_text {
						title = "Options",
						width_in_chars = LW,
					},
					f:checkbox {
						title = "Monochrome gain map",
						value = bind { key = K.monochromeGainmap, object = propertyTable },
					},
				},
				f:row {
					f:static_text {
						title = " ",
						width_in_chars = LW,
					},
					f:checkbox {
						title = "Keep HDR TIFF temp files",
						value = bind { key = K.keepIntermediates, object = propertyTable },
					},
				},
				f:row {
					f:static_text {
						title = " ",
						width_in_chars = LW,
					},
					f:checkbox {
						title = "Save debug copies (_uhdr_sdr / _uhdr_hdr next to output, verbose log)",
						value = bind { key = K.debugSaveArtifacts, object = propertyTable },
					},
				},
				f:row {
					f:static_text {
						title = " ",
						width_in_chars = LW,
					},
					f:checkbox {
						title = "Run --inspect on output (log)",
						value = bind { key = K.runInspect, object = propertyTable },
					},
				},
			},
		},
	}
end

--- Recursive delete of temp HDR folder (LrFileUtils, no shell rm).
local function safeDeleteTree(dir)
	if not dir or not LrFileUtils.exists(dir) then
		return
	end
	if LrFileUtils.recursiveFiles then
		pcall(function()
			for filePath in LrFileUtils.recursiveFiles(dir) do
				pcall(function()
					LrFileUtils.delete(filePath)
				end)
			end
		end)
	end
	pcall(function()
		LrFileUtils.delete(dir)
	end)
end

local function taskSleep(seconds)
	if LrTasks.sleep then
		LrTasks.sleep(seconds)
	elseif CMD.isWindows() then
		LrTasks.execute("ping -n 1 127.0.0.1 >nul")
	else
		LrTasks.execute("/bin/sleep " .. tostring(seconds))
	end
end

--- Normalize LrTasks.execute status (POSIX often returns exit*256; Windows returns exit directly).
local function shellExitStatus(st)
	if type(st) ~= "number" then
		return st
	end
	if CMD.isWindows() then
		if st > 0 and st <= 255 then
			return st
		end
		return st
	end
	if st > 0 and st <= 255 then
		return st
	end
	return math.floor(st / 256)
end

local function uhdrFailureHint(sx, rawSt)
	if CMD.isWindows() then
		if sx == 1 then
			return "\n\nEncoder exited with code 1 (usage or shell error). Check the log above for uhdr_repack output — if the log has no encoder lines after Command:, the command line may have been mangled (common with non-ASCII paths before staging). Update to the latest plug-in build."
		end
		if sx == 3 or sx == 4 then
			return "\n\nCould not read HDR TIFF or SDR base. Check the log for WIC or file errors (paths with non-ASCII characters such as Cyrillic are supported when the encoder receives them correctly)."
		end
		if sx == 137 or sx == -1073741571 then
			return "\n\nThe encoder may have been stopped by the system (memory pressure is common for large HDR TIFFs). Try reducing Image Sizing, closing other apps, or run the same command from Command Prompt to see a live error."
		end
		return ""
	end
	if sx == 137 or rawSt == 35072 then
		return "\n\nThis exit code often means the encoder was stopped by the system (memory pressure is common for large HDR TIFFs). Try reducing Image Sizing, closing other apps, or run the same command from Terminal to see a live error. You can also use Activity Monitor to check memory while encoding."
	end
	return ""
end

local function fileSizeBytes(path)
	local f = io.open(path, "rb")
	if not f then
		return nil
	end
	local n = f:seek("end")
	f:close()
	return n
end

--- Classic / BigTIFF little- or big-endian magic at offset 0.
local function tiffHeaderLooksValid(path)
	local f = io.open(path, "rb")
	if not f then
		return false
	end
	local h = f:read(8)
	f:close()
	if not h or #h < 4 then
		return false
	end
	local a, b, c, d = string.byte(h, 1, 4)
	if a == 0x49 and b == 0x49 and c == 0x2A and d == 0x00 then
		return true
	end
	if a == 0x4D and b == 0x4D and c == 0x00 and d == 0x2A then
		return true
	end
	if a == 0x49 and b == 0x49 and c == 0x2B and d == 0x00 then
		return true
	end
	if a == 0x4D and b == 0x4D and c == 0x00 and d == 0x2B then
		return true
	end
	return false
end

--- JPEG SOI marker (first bytes).
local function jpegHeaderLooksValid(path)
	local f = io.open(path, "rb")
	if not f then
		return false
	end
	local h = f:read(3)
	f:close()
	if not h or #h < 3 then
		return false
	end
	local a, b, c = string.byte(h, 1, 3)
	return a == 0xFF and b == 0xD8 and c == 0xFF
end

--[[
  Lightroom sometimes returns from waitForRender before the file is fully flushed.
  Wait until byte size is stable across two reads and the TIFF header is valid.
]]
local function waitForSettledHdrTiff(path, logPath)
	local lastSz
	local stable = 0
	local maxRounds = 40
	for i = 1, maxRounds do
		if not LrFileUtils.exists(path) then
			if logPath then
				Log.append(logPath, "HDR TIFF wait: file missing at round " .. tostring(i) .. "\n")
			end
			return false
		end
		local sz = fileSizeBytes(path)
		if sz and sz >= 8 and tiffHeaderLooksValid(path) then
			if lastSz == sz then
				stable = stable + 1
				if stable >= 2 then
					if logPath then
						Log.append(
							logPath,
							"HDR TIFF ready: " .. tostring(sz) .. " bytes after " .. tostring(i) .. " settle checks\n"
						)
					end
					return true
				end
			else
				stable = 0
				lastSz = sz
			end
		else
			stable = 0
			lastSz = sz
		end
		taskSleep(0.15)
	end
	if logPath then
		local sz = fileSizeBytes(path)
		local jpegHint = ""
		if sz and sz >= 3 and jpegHeaderLooksValid(path) then
			jpegHint = " (file starts with JPEG markers — internal pass wrote JPEG, not TIFF)\n"
		end
		Log.append(
			logPath,
			"HDR TIFF settle failed (last size="
				.. tostring(sz)
				.. ", header_ok="
				.. tostring(sz and sz >= 8 and tiffHeaderLooksValid(path))
				.. ")"
				.. jpegHint
		)
	end
	return false
end

--- SDR and HDR debug filenames next to the final export (same folder as basePath).
local function debugPostfixPaths(basePath, hdrPath)
	local folder = LrPathUtils.parent(basePath)
	local leaf = LrPathUtils.leafName(basePath)
	local baseNoExt = LrPathUtils.removeExtension(leaf)
	local baseExt = LrPathUtils.extension(basePath) or ""
	if baseExt == "" then
		baseExt = "jpg"
	end
	local hdrExt = LrPathUtils.extension(hdrPath) or "tif"
	local sdrDest = LrPathUtils.child(folder, baseNoExt .. "_uhdr_sdr." .. baseExt)
	local hdrDest = LrPathUtils.child(folder, baseNoExt .. "_uhdr_hdr." .. hdrExt)
	return sdrDest, hdrDest
end

-- LrPathUtils.extension returns the suffix without a leading dot (e.g. "tif" on macOS).
local function isTiffPath(p)
	if not p or type(p) ~= "string" then
		return false
	end
	local ext = string.lower(LrPathUtils.extension(p) or "")
	ext = string.gsub(ext, "^%.", "")
	if ext == "tif" or ext == "tiff" then
		return true
	end
	local leaf = string.lower(LrPathUtils.leafName(p) or "")
	return string.sub(leaf, -4) == ".tif" or string.sub(leaf, -5) == ".tiff"
end

local function isJpegPath(p)
	if not p or type(p) ~= "string" then
		return false
	end
	local ext = string.lower(LrPathUtils.extension(p) or "")
	ext = string.gsub(ext, "^%.", "")
	if ext == "jpg" or ext == "jpeg" then
		return true
	end
	local leaf = string.lower(LrPathUtils.leafName(p) or "")
	return string.sub(leaf, -4) == ".jpg" or string.sub(leaf, -5) == ".jpeg"
end

local function renderHdrTiff(photo, propertyTable, tempDir, logPath)
	local hdrSettings = UHDR.mergeHdrTiffSettings(propertyTable, tempDir)
	if logPath then
		pcall(function()
			if photo and photo.getDevelopSettings then
				local d = photo:getDevelopSettings()
				if d then
					Log.append(
						logPath,
						"Develop HDREditMode="
							.. tostring(d.HDREditMode)
							.. " (HDR editing must be on for HDR TIFF export)\n"
					)
				end
			end
		end)
		Log.append(
			logPath,
			string.format(
				"HDR pass settings: format=%s destType=%s dest=%s colorSpace=%s provider=%s\n",
				tostring(hdrSettings.LR_format),
				tostring(hdrSettings.LR_export_destinationType),
				tostring(hdrSettings.LR_export_destinationPathPrefix),
				tostring(hdrSettings.LR_export_colorSpace),
				tostring(hdrSettings.LR_exportServiceProvider)
			)
		)
	end
	local sess = LrExportSession({
		photosToExport = { photo },
		exportSettings = hdrSettings,
	})
	sess:doExportOnCurrentTask()

	local hdrPath
	local tried = {}
	local renderErrors = {}
	for _, rendition in sess:renditions() do
		local ok, pth = rendition:waitForRender()
		if ok and pth and pth ~= "" then
			tried[#tried + 1] = pth
			if isTiffPath(pth) then
				hdrPath = pth
				break
			end
		elseif not ok then
			local err = tostring(pth)
			renderErrors[#renderErrors + 1] = err
			if logPath then
				Log.append(logPath, "HDR TIFF waitForRender failed: " .. err .. "\n")
			end
		elseif logPath then
			Log.append(
				logPath,
				"HDR TIFF waitForRender: ok but empty path (ok=" .. tostring(ok) .. ").\n"
			)
		end
	end

	if not hdrPath then
		if logPath and #tried > 0 then
			Log.append(
				logPath,
				"HDR TIFF pass found no .tif rendition; paths returned: "
					.. table.concat(tried, "; ")
					.. "\n"
			)
		end
		if #tried > 0 then
			local first = tried[1]
			if isJpegPath(first) then
				return nil,
					"Internal HDR pass saved JPEG ("
						.. tostring(LrPathUtils.leafName(first))
						.. ") instead of TIFF. In Develop, turn HDR ON (Basics panel). Check the log line Develop HDREditMode= — it must indicate HDR editing is active. Otherwise Lightroom exports SDR JPEG even when the plug-in requests Rec2020 HDR TIFF."
			end
			return nil, "Internal HDR pass returned no TIFF file (got " .. tostring(first) .. ")."
		end
		if logPath then
			Log.append(logPath, "HDR TIFF session produced no rendition path.\n")
		end
		if #renderErrors > 0 then
			return nil, table.concat(renderErrors, " ")
		end
		return nil, ""
	end

	if logPath then
		Log.append(logPath, "HDR TIFF render: " .. tostring(hdrPath) .. "\n")
		if #tried > 1 then
			Log.append(logPath, "HDR TIFF rendition candidates: " .. table.concat(tried, "; ") .. "\n")
		end
	end

	if not LrFileUtils.exists(hdrPath) then
		if logPath then
			Log.append(logPath, "ERROR: HDR TIFF path missing on disk: " .. tostring(hdrPath) .. "\n")
		end
		if #renderErrors > 0 then
			return nil, table.concat(renderErrors, " ")
		end
		return nil, "exported path not found: " .. tostring(hdrPath)
	end

	if not waitForSettledHdrTiff(hdrPath, logPath) then
		if logPath then
			Log.append(logPath, "ERROR: HDR TIFF did not settle with a valid TIFF header (incomplete write?).\n")
		end
		return nil, "TIFF file did not finish writing or has an invalid header."
	end

	return hdrPath
end

function ExportHDRFilterProvider.postProcessRenderedPhotos(functionContext, filterContext)
	LrDialogs.attachErrorDialogToFunctionContext(functionContext)

	local propertyTable = filterContext.propertyTable
	UHDR.applyDefaults(propertyTable)

	local err = UHDR.validate(propertyTable)
	if err then
		LrDialogs.message("Ultra HDR", err, "warning")
		error(err)
	end

	local binary = CMD.bundledBinaryPath()
	if not CMD.binaryExists(binary) then
		local msg = "uhdr_repack not found at:\n"
			.. binary
			.. "\n\n"
			.. CMD.bundleInstructions()
		LrDialogs.message("Ultra HDR", msg, "critical")
		error(msg)
	end

	local destDir = propertyTable.LR_export_destinationPathPrefix
		or LrPathUtils.getStandardFilePath("temp")
	local sessionStamp = os.time()
	local logPath
	local currentLogFolder
	local altLogIndex = 0

	local function appendBinaryLine(path)
		if path then
			Log.append(path, "Binary: " .. tostring(binary) .. "\n")
		end
	end

	local function fallbackLog()
		if not logPath then
			logPath = Log.newSessionLog(destDir, "uhdr", sessionStamp)
			appendBinaryLine(logPath)
		end
		return logPath
	end

	--- Session log lives next to exported files (parent of basePath). If the batch uses multiple folders, open uhdr_export_<stamp>_<n>.log in each folder.
	local function ensureLogNextToBase(basePath)
		local folder = LrPathUtils.parent(basePath)
		if not folder or folder == "" then
			return fallbackLog()
		end
		if not logPath then
			logPath = Log.newSessionLog(folder, "uhdr", sessionStamp)
			currentLogFolder = folder
			appendBinaryLine(logPath)
		elseif folder ~= currentLogFolder then
			altLogIndex = altLogIndex + 1
			logPath = Log.newSessionLog(folder, "uhdr", sessionStamp .. "_" .. tostring(altLogIndex))
			currentLogFolder = folder
			appendBinaryLine(logPath)
		end
		return logPath
	end

	local progress = LrProgressScope({
		functionContext = functionContext,
		title = FILTER_UI_TITLE,
	})

	-- Optional: tie into Lightroom export progress when supported.
	if type(filterContext.configureProgress) == "function" then
		pcall(function()
			filterContext:configureProgress({
				title = FILTER_UI_TITLE,
				renderPortion = 1,
			})
		end)
	end

	math.randomseed(os.time() + math.floor((os.clock() or 0) * 1000000 % 999983))

	local done = 0
	for sourceRendition, _renditionToSatisfy in filterContext:renditions() do
		done = done + 1
		local canceled = false
		pcall(function()
			if progress.isCanceled and progress:isCanceled() then
				canceled = true
			end
		end)
		if canceled then
			break
		end

		local renderOk, basePathOrMsg = sourceRendition:waitForRender()
		if not renderOk then
			local msg = "Failed to render base export: " .. tostring(basePathOrMsg)
			Log.append(fallbackLog(), msg .. "\n")
			error(msg)
		end

		local basePath = basePathOrMsg
		logPath = ensureLogNextToBase(basePath)
		pcall(function()
			if progress.setCaption then
				progress:setCaption(LrPathUtils.leafName(basePath))
			end
		end)

		local photo = sourceRendition.photo
		if not photo then
			error("Ultra HDR: missing photo for rendition.")
		end

		Log.append(logPath, "\n--- Photo ---\nBase export: " .. tostring(basePath) .. "\n")
		if propertyTable[UHDR.KEY.debugSaveArtifacts] then
			Log.append(
				logPath,
				"LR_export_destinationPathPrefix: "
					.. tostring(propertyTable.LR_export_destinationPathPrefix)
					.. "\n"
			)
			pcall(function()
				local dim = photo:getFormattedMetadata("dimensions")
				if dim then
					Log.append(logPath, "dimensions (formatted): " .. tostring(dim) .. "\n")
				end
			end)
		end

		local tempRoot = LrPathUtils.getStandardFilePath("temp")
		local tempDir = LrPathUtils.child(
			tempRoot,
			string.format("uhdr_hdr_%s_%06d_%04d", os.time(), math.random(0, 999999), done)
		)
		LrFileUtils.createDirectory(tempDir)

		local hdrPath, hdrFailDetail = renderHdrTiff(photo, propertyTable, tempDir, logPath)
		if not hdrPath or not LrFileUtils.exists(hdrPath) then
			safeDeleteTree(tempDir)
			local detail = ""
			if hdrFailDetail and hdrFailDetail ~= "" then
				detail = "\nLightroom said: " .. hdrFailDetail .. "\n"
			end
			error(
				"Ultra HDR: HDR TIFF render failed. Requires Lightroom Classic 14+ with HDR editing/export support.\n"
					.. "If your catalog photo is not an HDR edit, enable HDR in Develop or adjust export compatibility.\n"
					.. detail
					.. "See plug-in README troubleshooting."
			)
		end

		if not isTiffPath(hdrPath) then
			Log.append(
				logPath,
				"ERROR: internal HDR pass must write a .tif file; got: " .. tostring(hdrPath) .. "\n"
			)
			if not propertyTable[UHDR.KEY.keepIntermediates] then
				safeDeleteTree(tempDir)
			end
			error(
				"Ultra HDR: internal HDR pass output is not a TIFF file: " .. tostring(hdrPath)
			)
		end

		if propertyTable[UHDR.KEY.debugSaveArtifacts] then
			local sdrDup, hdrDup = debugPostfixPaths(basePath, hdrPath)
			pcall(function()
				LrFileUtils.copy(basePath, sdrDup)
			end)
			pcall(function()
				LrFileUtils.copy(hdrPath, hdrDup)
			end)
			Log.append(logPath, "Debug: SDR copy: " .. tostring(sdrDup) .. "\n")
			Log.append(logPath, "Debug: HDR copy: " .. tostring(hdrDup) .. "\n")
		end

		local outPath = basePath

		-- ASCII-only staging paths for cmd.exe (avoids Cyrillic mangling in LrTasks.execute).
		local encodeHdrPath = LrPathUtils.child(tempDir, "uhdr_hdr_encode.tif")
		local hdrCopyOk = pcall(function()
			LrFileUtils.copy(hdrPath, encodeHdrPath)
		end)
		if not hdrCopyOk or not LrFileUtils.exists(encodeHdrPath) then
			safeDeleteTree(tempDir)
			error("Ultra HDR: could not copy HDR TIFF for encoding.")
		end
		Log.append(logPath, "Encode staging HDR: " .. tostring(encodeHdrPath) .. "\n")

		local baseExt = LrPathUtils.extension(basePath) or "jpg"
		local encodeBasePath = LrPathUtils.child(tempDir, "uhdr_sdr_base_copy." .. baseExt)
		local sdrCopyOk = pcall(function()
			LrFileUtils.copy(basePath, encodeBasePath)
		end)
		if not sdrCopyOk or not LrFileUtils.exists(encodeBasePath) then
			safeDeleteTree(tempDir)
			error("Ultra HDR: could not copy SDR base for encoding.")
		end
		Log.append(logPath, "Encode staging SDR: " .. tostring(encodeBasePath) .. "\n")

		if UHDR.sliceAspectEnabled(propertyTable) then
			Log.append(
				logPath,
				"Slicing enabled ("
					.. tostring(propertyTable[UHDR.KEY.sliceAspect])
					.. ")\n"
			)
		end

		local cmdLine = CMD.buildEncodeCommand({
			binary = binary,
			hdrTiff = encodeHdrPath,
			basePath = encodeBasePath,
			outPath = outPath,
			props = propertyTable,
		})

		Log.append(logPath, "Command: " .. cmdLine .. "\n")
		local st = CMD.runShell(cmdLine, logPath)
		if st ~= 0 then
			local sx = shellExitStatus(st)
			local hint = uhdrFailureHint(sx, st)
			Log.append(
				logPath,
				"uhdr_repack exit status: raw=" .. tostring(st) .. " exit≈" .. tostring(sx) .. "\n"
			)
			if hint ~= "" then
				Log.append(logPath, hint)
			end
			safeDeleteTree(tempDir)
			error(
				"uhdr_repack failed (exit "
					.. tostring(sx)
					.. ", raw "
					.. tostring(st)
					.. "). See log: "
					.. tostring(logPath)
					.. hint
			)
		end

		if propertyTable[UHDR.KEY.runInspect] then
			local ins = CMD.buildInspectCommand(binary, outPath)
			Log.append(logPath, "Inspect: " .. ins .. "\n")
			CMD.runShell(ins, logPath)
			if UHDR.sliceAspectEnabled(propertyTable) then
				local aspect = propertyTable[UHDR.KEY.sliceAspect]
				local slicePaths = CMD.listSliceOutputs(outPath, aspect)
				Log.append(logPath, "Slice inspect: found " .. tostring(#slicePaths) .. " file(s)\n")
				for _, slicePath in ipairs(slicePaths) do
					local sliceIns = CMD.buildInspectCommand(binary, slicePath)
					Log.append(logPath, "Inspect slice: " .. sliceIns .. "\n")
					CMD.runShell(sliceIns, logPath)
				end
			end
		end

		if not propertyTable[UHDR.KEY.keepIntermediates] then
			safeDeleteTree(tempDir)
		else
			Log.append(logPath, "Kept intermediate HDR TIFF folder: " .. tostring(tempDir) .. "\n")
		end
	end

	pcall(function()
		progress:done()
	end)
end

return ExportHDRFilterProvider
