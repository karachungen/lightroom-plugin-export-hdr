--[[----------------------------------------------------------------------------
  Loader for plug-in Lua modules. Lightroom `import` only resolves Lr* SDK
  namespaces, not sibling .lua files -- use loadfile from _PLUGIN.path instead.
  One global registry so all chunks share the same module cache.
----------------------------------------------------------------------------]]

local LrPathUtils = import "LrPathUtils"

local GKEY = "_UltraHDR_loadPluginModule"
local existing = rawget(_G, GKEY)
if existing then
	return existing
end

local cache = {}

local function loadPluginModule(nameNoExt)
	if cache[nameNoExt] then
		return cache[nameNoExt]
	end
	local path = LrPathUtils.child(_PLUGIN.path, nameNoExt .. ".lua")
	local chunk, err = loadfile(path)
	if not chunk then
		error("Ultra HDR Export: cannot load " .. path .. ": " .. tostring(err))
	end
	local mod = chunk()
	cache[nameNoExt] = mod
	return mod
end

rawset(_G, GKEY, loadPluginModule)
return loadPluginModule
