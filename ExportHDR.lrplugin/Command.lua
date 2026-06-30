--[[----------------------------------------------------------------------------
  Shell command construction and execution helpers for uhdr_repack.
----------------------------------------------------------------------------]]

local LrFileUtils = import "LrFileUtils"
local LrPathUtils = import "LrPathUtils"
local LrTasks = import "LrTasks"

local loadPluginModule = assert(loadfile(LrPathUtils.child(_PLUGIN.path, "PluginInit.lua")))()
local UHDR = loadPluginModule("UHDRSettings")

local CMD = {}

--- Escape a single argument for /bin/sh -c style quoting (macOS).
function CMD.shellQuote(path)
	if not path then
		return "''"
	end
	return "'" .. string.gsub(path, "'", "'\\''") .. "'"
end

--[[
  Bundled encoder (macOS 26 (Tahoe), ARM64); see scripts/bundle_uhdr_for_plugin.sh.
]]
function CMD.defaultBinaryPath()
	if not _PLUGIN or not _PLUGIN.path then
		return "bin/uhdr_repack"
	end
	return LrPathUtils.child(_PLUGIN.path, "bin/uhdr_repack")
end

--- Always use bundled `bin/uhdr_repack` under the plugin bundle.
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

--- Build main encode command string (returns full line for LrTasks.execute).
function CMD.buildEncodeCommand(o)
	-- o: { binary, hdrTiff, basePath, outPath, props from UHDRSettings keys }
	local K = UHDR.KEY
	local parts = {
		CMD.shellQuote(o.binary),
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
	local ext = LrPathUtils.extension(baseOutPath) or "jpg"
	local glob = LrPathUtils.child(folder, baseNoExt .. "_" .. aspect .. "_*." .. ext)
	local handle = io.popen("/bin/ls -1 " .. CMD.shellQuote(glob) .. " 2>/dev/null")
	if handle then
		for line in handle:lines() do
			if line and line ~= "" then
				paths[#paths + 1] = line
			end
		end
		handle:close()
	end
	table.sort(paths)
	return paths
end

function CMD.buildInspectCommand(binary, jpegPath)
	return table.concat({
		CMD.shellQuote(binary),
		"--inspect",
		CMD.shellQuote(jpegPath),
	}, " ")
end

--- Run via shell; returns exit status (number). Optional logPath appends stdout/stderr.
function CMD.runShell(line, logPath)
	if logPath and logPath ~= "" then
		local redir = " >> " .. CMD.shellQuote(logPath) .. " 2>&1"
		line = line .. redir
	end
	return LrTasks.execute(line)
end

function CMD.binaryExists(path)
	return path and path ~= "" and LrFileUtils.exists(path)
end

return CMD
