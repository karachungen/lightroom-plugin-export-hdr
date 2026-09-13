--[[----------------------------------------------------------------------------
  Export destination: renders SDR JPEGs, opens Ultra HDR, and renders HDR TIFF
  on demand (Gain/HDR tab or Encode) before writing Ultra HDR JPEGs.
----------------------------------------------------------------------------]]

local LrView = import "LrView"
local LrDialogs = import "LrDialogs"
local LrHttp = import "LrHttp"
local LrTasks = import "LrTasks"
local LrProgressScope = import "LrProgressScope"
local LrPathUtils = import "LrPathUtils"
local LrFileUtils = import "LrFileUtils"
local LrExportSession = import "LrExportSession"

local loadPluginModule = assert(loadfile(LrPathUtils.child(_PLUGIN.path, "PluginInit.lua")))()
local UHDR = loadPluginModule("UHDRSettings")
local CMD = loadPluginModule("Command")
local Log = loadPluginModule("Log")
local PreviewSession = loadPluginModule("PreviewSession")

local ExportHDRServiceProvider = {}

--- Export To destination, dialog section, and progress (distinct from LrPluginName in Info.lua).
local EXPORT_UI_TITLE = "Ultra HDR"
local ISSUES_URL = "https://github.com/karachungen/lightroom-plugin-export-hdr/issues"
local windowsNoticeShown = false

local function showWindowsUntestedNotice()
	if not CMD.isWindows() or windowsNoticeShown then
		return
	end
	windowsNoticeShown = true
	local result = LrDialogs.confirm(
		"Version 3 is only tested on macOS.",
		"Windows may have issues. If something breaks, please open a GitHub issue:\n" .. ISSUES_URL,
		"Continue",
		"Open issues"
	)
	if result == "cancel" then
		LrHttp.openUrlInBrowser(ISSUES_URL)
	end
end

ExportHDRServiceProvider.hideSections = { "video" }
ExportHDRServiceProvider.allowFileFormats = { "JPEG" }
ExportHDRServiceProvider.canExportVideo = false

--- Build exportPresetFields from defaults (preset persistence).
local exportPresetFields = {}
do
	for k, v in pairs(UHDR.defaults()) do
		exportPresetFields[#exportPresetFields + 1] = { key = k, default = v }
	end
end
ExportHDRServiceProvider.exportPresetFields = exportPresetFields

function ExportHDRServiceProvider.startDialog(propertyTable)
	UHDR.applyDefaults(propertyTable)
	UHDR.forceOverwriteExistingFiles(propertyTable)
	-- Keep Existing Files on overwrite so Lightroom does not ask on re-export.
	if propertyTable and not propertyTable._uhdrOverwriteObserver then
		propertyTable._uhdrOverwriteObserver = true
		pcall(function()
			propertyTable:addObserver("LR_collisionHandling", function(props, _key, value)
				if value ~= "overwrite" then
					props.LR_collisionHandling = "overwrite"
				end
			end)
		end)
	end
	showWindowsUntestedNotice()
end

function ExportHDRServiceProvider.updateExportSettings(exportSettings)
	UHDR.applyDefaults(exportSettings)
	UHDR.forceOverwriteExistingFiles(exportSettings)
end

function ExportHDRServiceProvider.sectionsForTopOfDialog(f, propertyTable)
	UHDR.applyDefaults(propertyTable)
	UHDR.forceOverwriteExistingFiles(propertyTable)
	local bind = LrView.bind
	local K = UHDR.KEY

	local howTo = {
		"How to use:",
		"In Export To, choose ULTRA HDR.",
		"File Settings are JPEG for the SDR base. Image Sizing applies to both passes.",
		"The plug-in writes SDR JPEGs and opens Ultra HDR. HDR TIFF is rendered when you open Gain/HDR for a photo, or when you click Encode.",
		"Existing files at the export path are always overwritten (no Ask / Skip prompt).",
		"Use HDR editing in Develop when needed (Lightroom 14+).",
	}
	if CMD.isWindows() then
		howTo[#howTo + 1] = "Version 3 is only tested on macOS. Windows may have issues — open a GitHub issue: "
			.. ISSUES_URL
	end

	return {
		{
			title = EXPORT_UI_TITLE,
			synopsis = "Exports SDR JPEGs, then opens Ultra HDR. HDR TIFF and encode run per photo on Gain/HDR preview or on Encode.",
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
							title = table.concat(howTo, "\n"),
							height_in_lines = #howTo + 1,
						},
					},
					f:spacer { height = f:control_spacing() },
					f:row {
						f:static_text {
							title = "Options",
							width_in_chars = 12,
						},
						f:checkbox {
							title = "Keep HDR TIFF temp files after export",
							value = bind { key = K.keepIntermediates, object = propertyTable },
						},
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

local function uhdrFailureHint(sx, rawSt, logPath)
	if CMD.isWindows() then
		if sx == 1 then
			local logTail = ""
			if logPath and logPath ~= "" then
				local f = io.open(logPath, "rb")
				if f then
					logTail = f:read("*a") or ""
					f:close()
				end
			end
			local shellNotFound = string.find(logTail, "internal or external command", 1, true)
				or string.find(logTail, "внутренней или внешней", 1, true)
				or string.find(logTail, "не является", 1, true)
			local noEncoderOutput = string.find(logTail, "Command:", 1, true)
				and not string.find(logTail, "dimensions:", 1, true)
				and not string.find(logTail, "Wrote ", 1, true)
			if shellNotFound or noEncoderOutput then
				return "\n\nThe encoder likely never ran (Windows cmd.exe mangled the command). Common causes: plug-in installed under a folder with spaces or parentheses (e.g. Downloads\\… (1)), or non-ASCII characters in paths passed through cmd. This build stages HDR/SDR/output through ASCII temp paths and uses a relative uhdr_repack.exe — update the plug-in if you still see this."
			end
			return "\n\nEncoder exited with code 1 (usage or shell error). Check the log above for uhdr_repack output."
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

--- Promote staged encoder output to the Lightroom export path (Windows staging only).
local function pathEqual(a, b)
	if not a or not b then
		return false
	end
	a = string.gsub(tostring(a), "\\", "/")
	b = string.gsub(tostring(b), "\\", "/")
	if CMD.isWindows() then
		return string.lower(a) == string.lower(b)
	end
	return a == b
end

local function promoteEncodedFile(src, dest, logPath, sdrSizeBytes)
	if not src or not dest or not LrFileUtils.exists(src) then
		return false, "source missing"
	end
	local srcSize = fileSizeBytes(src)
	if not srcSize or srcSize <= 0 or not jpegHeaderLooksValid(src) then
		return false, "invalid staged JPEG"
	end
	if LrFileUtils.exists(dest) then
		pcall(function()
			LrFileUtils.delete(dest)
		end)
	end
	local copyOk = pcall(function()
		LrFileUtils.copy(src, dest)
	end)
	if not copyOk or not LrFileUtils.exists(dest) then
		return false, "copy failed"
	end
	local destSize = fileSizeBytes(dest)
	if logPath then
		Log.append(
			logPath,
			"Promote: src=" .. tostring(srcSize) .. " bytes dest=" .. tostring(destSize) .. " bytes\n"
		)
	end
	if not destSize or destSize <= 0 then
		return false, "destination empty"
	end
	if sdrSizeBytes and srcSize > sdrSizeBytes and destSize == sdrSizeBytes then
		return false, "destination still matches SDR size after promote"
	end
	if destSize ~= srcSize then
		return false, "destination size mismatch after promote"
	end
	return true
end

--- Promote a staged slice JPEG to the export folder (Windows staging only).
local function promoteSliceFile(src, dest, logPath)
	if not src or not dest or not LrFileUtils.exists(src) then
		return false
	end
	if LrFileUtils.exists(dest) then
		pcall(function()
			LrFileUtils.delete(dest)
		end)
	end
	local copyOk = pcall(function()
		LrFileUtils.copy(src, dest)
	end)
	if not copyOk or not LrFileUtils.exists(dest) then
		return false
	end
	local srcSize = fileSizeBytes(src)
	local destSize = fileSizeBytes(dest)
	if logPath and srcSize and destSize then
		Log.append(
			logPath,
			"Promote slice: src=" .. tostring(srcSize) .. " bytes dest=" .. tostring(destSize) .. " bytes\n"
		)
	end
	return srcSize and destSize and destSize == srcSize
end

--- Run uhdr_repack --inspect and return whether output reports Ultra HDR.
local function inspectIsUltraHdr(binary, path)
	if not path or not LrFileUtils.exists(path) then
		return false
	end
	local cmd = CMD.buildInspectCommand(binary, path)
	if CMD.isWindows() then
		-- io.popen also goes through `cmd /c`; resolve the relative exe name to the
		-- absolute bundled path and add sacrificial quotes (see CMD.wrapForWindowsShell).
		cmd = CMD.wrapForWindowsShell(CMD.resolveWindowsCommand(cmd) .. " 2>nul")
	else
		cmd = cmd .. " 2>/dev/null"
	end
	local handle = io.popen(cmd)
	if not handle then
		return false
	end
	local out = handle:read("*a") or ""
	handle:close()
	return string.find(out, "is_ultra_hdr: yes", 1, true) ~= nil
end

--- Fail export when final JPEG is not valid Ultra HDR (size + inspect).
local function assertFinalUltraHdr(binary, outPath, logPath, sdrSizeBytes)
	local outSize = fileSizeBytes(outPath)
	if logPath then
		Log.append(
			logPath,
			"Final output size: " .. tostring(outSize) .. " bytes (SDR base was " .. tostring(sdrSizeBytes) .. ")\n"
		)
	end
	if not outSize or outSize <= 0 or not jpegHeaderLooksValid(outPath) then
		error("Ultra HDR: encoder output is missing or not a JPEG: " .. tostring(outPath))
	end
	if sdrSizeBytes and outSize == sdrSizeBytes then
		error(
			"Ultra HDR: final export matches SDR base size — gain map was not written to "
				.. tostring(outPath)
		)
	end
	if not inspectIsUltraHdr(binary, outPath) then
		error("Ultra HDR: --inspect reports the final export is not Ultra HDR: " .. tostring(outPath))
	end
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

local function fulfillOnDemandHdrTiff(item, propertyTable, previewWorkRoot)
	if not item or not item.id then
		return
	end
	local logPath = item.logPath
	local okDone, existing = PreviewSession.doneHasValidTiff(previewWorkRoot, item.id)
	if okDone and existing then
		item.hdr_tiff = existing
		PreviewSession.writeDone(previewWorkRoot, item.id, true, existing, nil)
		return
	end
	if item.hdr_tiff and item.hdr_tiff ~= "" and LrFileUtils.exists(item.hdr_tiff) then
		PreviewSession.writeDone(previewWorkRoot, item.id, true, item.hdr_tiff, nil)
		PreviewSession.appendActivity(previewWorkRoot, item.id, "wait_tiff", "on disk " .. tostring(item.hdr_tiff))
		return
	end

	local function fail(msg)
		Log.append(logPath or "", "HDR TIFF request failed for " .. tostring(item.id) .. ": " .. tostring(msg) .. "\n")
		PreviewSession.appendActivity(previewWorkRoot, item.id, "wait_tiff", "fail " .. tostring(msg))
		PreviewSession.writeDone(previewWorkRoot, item.id, false, "", msg)
	end

	if not item.photo then
		fail("missing Lightroom photo object")
		return
	end
	if not item.tempDir then
		fail("missing temp directory")
		return
	end

	PreviewSession.appendActivity(previewWorkRoot, item.id, "wait_tiff", "Lightroom render start")
	local hdrPath, hdrFailDetail = renderHdrTiff(item.photo, propertyTable, item.tempDir, logPath)
	if not hdrPath or not LrFileUtils.exists(hdrPath) then
		local detail = hdrFailDetail or "HDR TIFF render failed"
		fail(
			"Ultra HDR: HDR TIFF render failed. Requires Lightroom Classic 14+ with HDR editing/export support. "
				.. tostring(detail)
		)
		return
	end
	if not isTiffPath(hdrPath) then
		fail("internal HDR pass output is not a TIFF file: " .. tostring(hdrPath))
		return
	end

	local encodeHdrPath = item.hdrStagingPath or LrPathUtils.child(item.tempDir, "uhdr_hdr_encode.tif")
	local hdrCopyOk = pcall(function()
		LrFileUtils.copy(hdrPath, encodeHdrPath)
	end)
	if not hdrCopyOk or not LrFileUtils.exists(encodeHdrPath) then
		fail("could not copy HDR TIFF for encoding")
		return
	end
	item.hdr_tiff = encodeHdrPath
	Log.append(logPath or "", "Encode staging HDR: " .. tostring(encodeHdrPath) .. "\n")
	PreviewSession.appendActivity(previewWorkRoot, item.id, "wait_tiff", "ready " .. tostring(encodeHdrPath))
	PreviewSession.writeDone(previewWorkRoot, item.id, true, encodeHdrPath, nil)
end

function ExportHDRServiceProvider.processRenderedPhotos(functionContext, exportContext)
	LrDialogs.attachErrorDialogToFunctionContext(functionContext)

	local propertyTable = exportContext.propertyTable
	UHDR.applyDefaults(propertyTable)
	UHDR.forceOverwriteExistingFiles(propertyTable)
	showWindowsUntestedNotice()

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
		title = EXPORT_UI_TITLE,
	})

	pcall(function()
		exportContext:configureProgress({
			title = EXPORT_UI_TITLE,
		})
	end)

	math.randomseed(os.time() + math.floor((os.clock() or 0) * 1000000 % 999983))

	local done = 0
	local previewBatch = {}
	local previewWorkRoot = LrPathUtils.child(
		LrPathUtils.getStandardFilePath("temp"),
		string.format("uhdr_preview_%s", tostring(sessionStamp))
	)
	LrFileUtils.createDirectory(previewWorkRoot)

	for _, rendition in exportContext:renditions({ stopIfCanceled = true }) do
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

		local renderOk, basePathOrMsg = rendition:waitForRender()
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

		local photo = rendition.photo
		if not photo then
			error("Ultra HDR: missing photo for rendition.")
		end

		Log.append(logPath, "\n--- Photo ---\nBase export: " .. tostring(basePath) .. "\n")

		local tempRoot = LrPathUtils.getStandardFilePath("temp")
		local tempDir = LrPathUtils.child(
			tempRoot,
			string.format("uhdr_hdr_%s_%06d_%04d", os.time(), math.random(0, 999999), done)
		)
		LrFileUtils.createDirectory(tempDir)

		local outPath = basePath
		local useStagingOut = CMD.isWindows()

		-- ASCII-only staging paths for cmd.exe (avoids Cyrillic / special-char mangling in LrTasks.execute).
		local encodeHdrPath = LrPathUtils.child(tempDir, "uhdr_hdr_encode.tif")

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

		local encodeOutPath
		if useStagingOut then
			encodeOutPath = LrPathUtils.child(tempDir, "uhdr_out_encode.jpg")
			Log.append(logPath, "Encode staging OUT: " .. tostring(encodeOutPath) .. "\n")
		else
			encodeOutPath = outPath
		end
		Log.append(logPath, "Final OUT: " .. tostring(outPath) .. "\n")

		previewBatch[#previewBatch + 1] = {
			id = "photo_" .. tostring(done),
			label = LrPathUtils.leafName(basePath) or ("Photo " .. tostring(done)),
			sdr = encodeBasePath,
			hdr_tiff = "",
			hdrStagingPath = encodeHdrPath,
			out = encodeOutPath,
			finalOut = outPath,
			tempDir = tempDir,
			logPath = logPath,
			useStagingOut = useStagingOut,
			sdrSize = fileSizeBytes(encodeBasePath),
			photo = photo,
		}
		Log.append(logPath, "Queued for Ultra HDR (HDR TIFF preload on editor open)\n")
	end

	if #previewBatch > 0 then
		local resultPath = LrPathUtils.child(previewWorkRoot, "result.json")
		local previewLog = fallbackLog()
		local sessionPath = PreviewSession.write(previewWorkRoot, resultPath, previewBatch, previewLog)
		PreviewSession.writeBridge(previewWorkRoot)
		PreviewSession.appendActivity(previewWorkRoot, "", "session", "editor start " .. tostring(sessionPath))
		Log.append(previewLog, "Preview session: " .. tostring(sessionPath) .. "\n")
		Log.append(previewLog, "Preview activity: " .. PreviewSession.previewLogPath(previewWorkRoot) .. "\n")
		local previewCmd = CMD.buildPreviewEditCommand(binary, sessionPath)
		Log.append(previewLog, "Preview command: " .. previewCmd .. "\n")

		local itemsById = {}
		for _, item in ipairs(previewBatch) do
			itemsById[item.id] = item
		end

		local guiDone = false
		local pst = 0
		if LrTasks.startAsyncTask then
			LrTasks.startAsyncTask(function()
				pst = CMD.runShell(previewCmd, previewLog)
				guiDone = true
			end)
			while not guiDone do
				local canceled = false
				pcall(function()
					if progress.isCanceled and progress:isCanceled() then
						canceled = true
					end
				end)
				if canceled then
					break
				end
				local reqId = PreviewSession.nextPendingRequestId(previewWorkRoot)
				if reqId then
					local item = itemsById[reqId]
					if item then
						pcall(function()
							if progress.setCaption then
								progress:setCaption("HDR: " .. (item.label or item.id))
							end
						end)
						fulfillOnDemandHdrTiff(item, propertyTable, previewWorkRoot)
					else
						PreviewSession.writeDone(previewWorkRoot, reqId, false, "", "unknown photo id")
					end
				end
				taskSleep(0.15)
			end
		else
			pst = CMD.runShell(previewCmd, previewLog)
		end

		local function discardPreviewTemps()
			for _, item in ipairs(previewBatch) do
				if not propertyTable[UHDR.KEY.keepIntermediates] then
					safeDeleteTree(item.tempDir)
				end
			end
		end

		local function previewCanceledFromLightroom()
			local canceled = false
			pcall(function()
				if progress.isCanceled and progress:isCanceled() then
					canceled = true
				end
			end)
			return canceled
		end

		local sx = shellExitStatus(pst)
		local approved = PreviewSession.readApproved(resultPath)
		-- Studio cancel returns 2 (LrTasks.execute: 512 on macOS). Do not surface that as an export error.
		if previewCanceledFromLightroom() or sx == 2 or (sx == 0 and not approved) then
			discardPreviewTemps()
			Log.append(previewLog, "Ultra HDR preview cancelled.\n")
			pcall(function()
				progress:done()
			end)
			return
		end
		if sx ~= 0 then
			discardPreviewTemps()
			error(
				"Ultra HDR preview failed (exit "
					.. tostring(sx)
					.. "). See log: "
					.. tostring(previewLog)
					.. uhdrFailureHint(sx, pst, previewLog)
			)
		end
		if not approved then
			discardPreviewTemps()
			Log.append(previewLog, "Ultra HDR preview cancelled.\n")
			pcall(function()
				progress:done()
			end)
			return
		end
		local itemStatuses = PreviewSession.readItemStatuses(resultPath)
		local destDir = PreviewSession.readDestDir(resultPath)
		for _, item in ipairs(previewBatch) do
			logPath = item.logPath or previewLog
			local status = itemStatuses[item.id]
			local encoded = status and status.encoded == true
			local skipped = status and status.skipped == true
			if skipped or not encoded then
				Log.append(
					logPath,
					"Skipping promote for preview item "
						.. tostring(item.id)
						.. " (skipped="
						.. tostring(skipped)
						.. ", encoded="
						.. tostring(encoded)
						.. ")\n"
				)
				if not propertyTable[UHDR.KEY.keepIntermediates] then
					safeDeleteTree(item.tempDir)
				end
			else
				local folder = destDir or LrPathUtils.parent(item.finalOut) or "."
				LrFileUtils.createDirectory(folder)
				local destFile = LrPathUtils.child(folder, LrPathUtils.leafName(item.finalOut) or "export.jpg")
				local srcFile = (status and status.out) or item.out
				if not pathEqual(srcFile, destFile) then
					local promoteOk, promoteErr = promoteEncodedFile(srcFile, destFile, logPath, item.sdrSize)
					if not promoteOk then
						error(
							"Ultra HDR: could not promote preview-encoded JPEG: "
								.. tostring(promoteErr)
								.. " ("
								.. tostring(destFile)
								.. ")"
						)
					end
					Log.append(logPath, "Promoted preview output to: " .. tostring(destFile) .. "\n")
				else
					Log.append(logPath, "Encoded in place: " .. tostring(destFile) .. "\n")
				end

				if status and status.slices then
					for _, stagedSlice in ipairs(status.slices) do
						local destSlice = LrPathUtils.child(folder, LrPathUtils.leafName(stagedSlice) or "")
						if destSlice and destSlice ~= "" and LrFileUtils.exists(stagedSlice) then
							if pathEqual(stagedSlice, destSlice) then
								Log.append(logPath, "Slice already at destination: " .. tostring(destSlice) .. "\n")
							elseif promoteSliceFile(stagedSlice, destSlice, logPath) then
								Log.append(logPath, "Promoted slice to: " .. tostring(destSlice) .. "\n")
							end
						end
					end
				end
				assertFinalUltraHdr(binary, destFile, logPath, item.sdrSize)
				if not propertyTable[UHDR.KEY.keepIntermediates] then
					safeDeleteTree(item.tempDir)
				end
			end
		end
	end

	pcall(function()
		progress:done()
	end)
end

return ExportHDRServiceProvider
