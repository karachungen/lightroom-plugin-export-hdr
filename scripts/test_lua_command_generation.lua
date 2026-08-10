-- Standalone regression test for Lightroom command generation (Lua 5.1+).
local repoRoot = assert(arg[1], "usage: lua test_lua_command_generation.lua <repo-root>")
local pluginRoot = repoRoot .. "/ExportHDR.lrplugin"

_PLUGIN = { path = pluginRoot }
WIN_ENV = false

local pathUtils = {}
function pathUtils.child(parent, child)
	return parent .. "/" .. child
end
function pathUtils.normalize(path)
	return path
end
function pathUtils.parent(path)
	return string.match(path, "^(.*)[/\\][^/\\]+$") or "."
end
function pathUtils.leafName(path)
	return string.match(path, "([^/\\]+)$") or path
end
function pathUtils.extension(path)
	return string.match(path, "%.([^%.]+)$")
end
function pathUtils.removeExtension(path)
	return (string.gsub(path, "%.[^%.]+$", ""))
end
function pathUtils.getStandardFilePath()
	return "."
end

function import(name)
	if name == "LrPathUtils" then
		return pathUtils
	elseif name == "LrFileUtils" then
		return {}
	elseif name == "LrTasks" then
		return {}
	end
	error("unexpected Lightroom import: " .. tostring(name))
end

local loadPluginModule = assert(loadfile(pluginRoot .. "/PluginInit.lua"))()
local UHDR = loadPluginModule("UHDRSettings")
local CMD = loadPluginModule("Command")
local props = UHDR.defaults()

assert(props[UHDR.KEY.autoContentBoost] == true, "Auto min/max boost must default ON")

local function command()
	return CMD.buildEncodeCommand({
		binary = "/tmp/uhdr_repack",
		hdrTiff = "/tmp/hdr.tif",
		basePath = "/tmp/sdr.jpg",
		outPath = "/tmp/out.jpg",
		props = props,
	})
end

local auto = command()
assert(not string.find(auto, "--min-content-boost", 1, true), "Auto command contains min flag")
assert(not string.find(auto, "--max-content-boost", 1, true), "Auto command contains max flag")

props[UHDR.KEY.autoContentBoost] = false
props[UHDR.KEY.minContentBoost] = 0.5
props[UHDR.KEY.maxContentBoost] = 8
local manual = command()
assert(string.find(manual, "--min-content-boost 0.5", 1, true), "Manual command lacks min flag")
assert(string.find(manual, "--max-content-boost 8", 1, true), "Manual command lacks max flag")

print("PASS: Lua Auto omits min/max flags; Manual emits both flags")
