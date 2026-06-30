--[[----------------------------------------------------------------------------
  Shell command construction and execution helpers for uhdr_repack.
  macOS: bin/uhdr_repack   Windows: bin/uhdr_repack.exe
----------------------------------------------------------------------------]]

local LrFileUtils = import "LrFileUtils"
local LrPathUtils = import "LrPathUtils"
local LrTasks = import "LrTasks"

local loadPluginModule = assert(loadfile(LrPathUtils.child(_PLUGIN.path, "PluginInit.lua")))()
local UHDR = loadPluginModule("UHDRSettings")

local CMD = {}

--- True when running in Lightroom Classic on Windows.
function CMD.isWindows()
	if WIN_ENV == true then
		return true
	end
	local sep = package.config:sub(1, 1)
	return sep == "\\"
end

function CMD.binaryFileName()
	if CMD.isWindows() then
		return "uhdr_repack.exe"
	end
	return "uhdr_repack"
end

--- Escape a single argument for the host shell (POSIX sh or Windows cmd).
function CMD.shellQuote(path)
	if not path then
		if CMD.isWindows() then
			return '""'
		end
		return "''"
	end
	if CMD.isWindows() then
		return '"' .. string.gsub(path, '"', '""') .. '"'
	end
	return "'" .. string.gsub(path, "'", "'\\''") .. "'"
end

--- Bundled encoder under ExportHDR.lrplugin/bin (platform-specific name).
function CMD.defaultBinaryPath()
	local name = CMD.binaryFileName()
	if not _PLUGIN or not _PLUGIN.path then
		return "bin/" .. name
	end
	return LrPathUtils.child(_PLUGIN.path, "bin/" .. name)
end

function CMD.bundledBinaryPath()
	local p = CMD.defaultBinaryPath()
	if LrPathUtils.normalize then
		local ok, n = pcall(function()
			return LrPathUtils.normalize(p)
		end)
		if ok and n and n ~= "" then
			return n
		end
	end
	return p
end

--- Encoder token for shell lines (relative name on Windows for logs; resolved in runShell).
function CMD.shellBinary(binary)
	if CMD.isWindows() then
		return CMD.binaryFileName()
	end
	return binary or CMD.bundledBinaryPath()
end

--- Build main encode command string (returns full line for LrTasks.execute).
function CMD.buildEncodeCommand(o)
	local K = UHDR.KEY
	local parts = {
		CMD.shellBinary(o.binary),
		"--hdr-tiff",
		CMD.shellQuote(o.hdrTiff),
		"--base",
		CMD.shellQuote(o.basePath),
		"--out",
		CMD.shellQuote(o.outPath),
		"--base-quality",
		tostring(math.floor(tonumber(o.props[K.baseQuality]) or 92)),
		"--gainmap-quality",
		tostring(math.floor(tonumber(o.props[K.gainmapQuality]) or 85)),
		"--gainmap-scale",
		tostring(math.floor(tonumber(o.props[K.gainmapScale]) or 1)),
		"--min-content-boost",
		tostring(tonumber(o.props[K.minContentBoost]) or 1.0),
		"--max-content-boost",
		tostring(tonumber(o.props[K.maxContentBoost]) or 1000.0),
		"--target-display-peak",
		tostring(tonumber(o.props[K.targetDisplayPeak]) or 1000.0),
	}
	if o.props[K.monochromeGainmap] then
		table.insert(parts, "--monochrome-gainmap")
	end
	local sliceAspect = o.props[K.sliceAspect]
	if sliceAspect and sliceAspect ~= "none" and sliceAspect ~= "" then
		table.insert(parts, "--slice-aspect")
		table.insert(parts, sliceAspect)
	end
	return table.concat(parts, " ")
end

local function normalizeFolderPath(folder)
	if not folder or folder == "" then
		return "."
	end
	if LrPathUtils.normalize then
		local ok, n = pcall(function()
			return LrPathUtils.normalize(folder)
		end)
		if ok and n and n ~= "" then
			return n
		end
	end
	return folder
end

local function fileMatchesSlicePattern(filePath, folder, prefix, extLower)
	if not filePath or filePath == "" then
		return false
	end
	local parent = LrPathUtils.parent(filePath)
	if normalizeFolderPath(parent) ~= normalizeFolderPath(folder) then
		return false
	end
	local leaf = LrPathUtils.leafName(filePath) or ""
	if string.sub(leaf, 1, #prefix) ~= prefix then
		return false
	end
	local fileExt = string.lower(LrPathUtils.extension(filePath) or "")
	fileExt = string.gsub(fileExt, "^%.", "")
	return fileExt == extLower
end

--- List numbered Ultra HDR slice outputs next to baseOutPath (e.g. photo_1x1_01.jpg).
function CMD.listSliceOutputs(baseOutPath, aspect)
	local paths = {}
	if not baseOutPath or not aspect or aspect == "none" then
		return paths
	end
	local folder = LrPathUtils.parent(baseOutPath)
	if not folder or folder == "" then
		folder = "."
	end
	local leaf = LrPathUtils.leafName(baseOutPath)
	local baseNoExt = LrPathUtils.removeExtension(leaf)
	local ext = string.lower(LrPathUtils.extension(baseOutPath) or "jpg")
	ext = string.gsub(ext, "^%.", "")
	local prefix = baseNoExt .. "_" .. aspect .. "_"

	if LrFileUtils.recursiveFiles then
		pcall(function()
			for filePath in LrFileUtils.recursiveFiles(folder) do
				if fileMatchesSlicePattern(filePath, folder, prefix, ext) then
					paths[#paths + 1] = filePath
				end
			end
		end)
	else
		local glob = LrPathUtils.child(folder, prefix .. "*." .. ext)
		local cmd
		if CMD.isWindows() then
			cmd = 'cmd /c dir /b ' .. CMD.shellQuote(glob) .. " 2>nul"
		else
			cmd = "/bin/ls -1 " .. CMD.shellQuote(glob) .. " 2>/dev/null"
		end
		local handle = io.popen(cmd)
		if handle then
			for line in handle:lines() do
				if line and line ~= "" then
					local full = line
					if not string.find(line, "[/\\]", 1) then
						full = LrPathUtils.child(folder, line)
					end
					paths[#paths + 1] = full
				end
			end
			handle:close()
		end
	end

	table.sort(paths)
	return paths
end

function CMD.buildInspectCommand(binary, jpegPath)
	return table.concat({
		CMD.shellBinary(binary),
		"--inspect",
		CMD.shellQuote(jpegPath),
	}, " ")
end

--- Map a staged slice path (next to stagedOutPath) to the final export folder/name.
function CMD.promoteStagedSlicePath(stagedSlicePath, stagedOutPath, finalOutPath)
	local stagedLeaf = LrPathUtils.leafName(stagedOutPath) or ""
	local stagedNoExt = LrPathUtils.removeExtension(stagedLeaf) or stagedLeaf
	local sliceLeaf = LrPathUtils.leafName(stagedSlicePath) or ""
	if string.sub(sliceLeaf, 1, #stagedNoExt) ~= stagedNoExt then
		return nil
	end
	local suffix = string.sub(sliceLeaf, #stagedNoExt + 1)
	local finalLeaf = LrPathUtils.leafName(finalOutPath) or ""
	local finalNoExt = LrPathUtils.removeExtension(finalLeaf) or finalLeaf
	local finalFolder = LrPathUtils.parent(finalOutPath) or "."
	return LrPathUtils.child(finalFolder, finalNoExt .. suffix)
end

function CMD.pluginBinDir()
	return LrPathUtils.parent(CMD.bundledBinaryPath())
end

--- Replace leading uhdr_repack.exe with quoted absolute path (Windows LrTasks.execute).
function CMD.resolveWindowsCommand(line)
	if not line or line == "" then
		return line
	end
	local binName = CMD.binaryFileName()
	if string.sub(line, 1, #binName) == binName then
		return CMD.shellQuote(CMD.bundledBinaryPath()) .. string.sub(line, #binName + 1)
	end
	return line
end

local function appendCaptureToLog(logPath, capturePath)
	if not logPath or logPath == "" or not capturePath then
		return
	end
	local cap = io.open(capturePath, "rb")
	if not cap then
		return
	end
	local content = cap:read("*a")
	cap:close()
	if not content or content == "" then
		pcall(function()
			LrFileUtils.delete(capturePath)
		end)
		return
	end
	local log = io.open(logPath, "a")
	if log then
		log:write(content)
		if string.sub(content, -1) ~= "\n" then
			log:write("\n")
		end
		log:close()
	end
	pcall(function()
		LrFileUtils.delete(capturePath)
	end)
end

--- Run via shell; returns exit status (number). Optional logPath appends stdout/stderr.
function CMD.runShell(line, logPath)
	if CMD.isWindows() then
		local tempRoot = LrPathUtils.getStandardFilePath("temp") or "."
		local capturePath = LrPathUtils.child(
			tempRoot,
			"uhdr_run_" .. tostring(os.time()) .. "_" .. tostring(math.random(100000, 999999)) .. ".txt"
		)
		local inner = CMD.resolveWindowsCommand(line)
		if logPath and logPath ~= "" then
			inner = inner .. " > " .. CMD.shellQuote(capturePath) .. " 2>&1"
		end
		local st = LrTasks.execute(inner)
		if logPath and logPath ~= "" then
			appendCaptureToLog(logPath, capturePath)
		end
		return st
	end
	if logPath and logPath ~= "" then
		line = line .. " >> " .. CMD.shellQuote(logPath) .. " 2>&1"
	end
	return LrTasks.execute(line)
end

function CMD.binaryExists(path)
	return path and path ~= "" and LrFileUtils.exists(path)
end

function CMD.bundleInstructions()
	if CMD.isWindows() then
		return "Run scripts\\bundle_uhdr_for_plugin_windows.ps1 (Windows x64) to install bin\\uhdr_repack.exe inside the plug-in bundle."
	end
	return "Run scripts/bundle_uhdr_for_plugin.sh (macOS 26 (Tahoe), ARM64) to install bin/uhdr_repack inside the plug-in bundle."
end

return CMD
