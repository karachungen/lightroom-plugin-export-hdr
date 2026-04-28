--[[----------------------------------------------------------------------------
  Simple file logger for export runs.
----------------------------------------------------------------------------]]

local LrFileUtils = import "LrFileUtils"
local LrPathUtils = import "LrPathUtils"

local Log = {}

function Log.append(path, text)
	if not path or path == "" then
		return
	end
	pcall(function()
		local h = io.open(path, "a")
		if h then
			h:write(text)
			h:close()
		end
	end)
end

function Log.timestamp()
	return os.date("!%Y-%m-%dT%H:%M:%SZ")
end

--- @param timeStamp optional unix time for stable filename across a batch (default os.time())
function Log.newSessionLog(parentDir, baseName, timeStamp)
	if not parentDir or parentDir == "" then
		return nil
	end
	local name = (baseName or "uhdr") .. "_export_" .. tostring(timeStamp or os.time()) .. ".log"
	local path = LrPathUtils.child(parentDir, name)
	pcall(function()
		LrFileUtils.createDirectory(parentDir)
		local h = io.open(path, "w")
		if h then
			h:write("Ultra HDR Export log — " .. Log.timestamp() .. "\n")
			h:close()
		end
	end)
	return path
end

return Log
