--[[----------------------------------------------------------------------------
  Write preview session.json for uhdr_repack --edit --session.
  Encoder settings are owned by Ultra HDR (web UI).
----------------------------------------------------------------------------]]

local LrFileUtils = import "LrFileUtils"
local LrPathUtils = import "LrPathUtils"

local PreviewSession = {}

local function jsonEscape(s)
	s = tostring(s or "")
	s = string.gsub(s, "\\", "\\\\")
	s = string.gsub(s, '"', '\\"')
	return s
end

--- @param workDir string absolute path for session work files
--- @param items table[] { id, label, sdr, hdr_tiff, out }
--- @return string sessionPath absolute path to written JSON
local function jsonUnescape(s)
	s = tostring(s or "")
	s = string.gsub(s, '\\"', '"')
	s = string.gsub(s, "\\\\", "\\")
	return s
end

function PreviewSession.previewLogPath(workDir)
	return LrPathUtils.child(workDir, "uhdr_preview.log")
end

function PreviewSession.appendActivity(workDir, photoId, phase, detail)
	local path = PreviewSession.previewLogPath(workDir)
	local stamp = os.date("!%Y-%m-%dT%H:%M:%SZ")
	local parts = { stamp }
	if photoId and photoId ~= "" then
		parts[#parts + 1] = tostring(photoId)
	end
	if phase and phase ~= "" then
		parts[#parts + 1] = tostring(phase)
	end
	if detail and detail ~= "" then
		parts[#parts + 1] = tostring(detail)
	end
	pcall(function()
		LrFileUtils.createDirectory(workDir)
		local h = io.open(path, "a")
		if h then
			h:write(table.concat(parts, " ") .. "\n")
			h:close()
		end
	end)
end

function PreviewSession.write(workDir, resultPath, items, logPath)
	LrFileUtils.createDirectory(workDir)

	local destDir = ""
	for _, item in ipairs(items or {}) do
		local named = item.finalOut or item.out
		if named and named ~= "" then
			destDir = LrPathUtils.parent(named) or ""
			if destDir and destDir ~= "" then
				break
			end
		end
	end

	local sessionPath = LrPathUtils.child(workDir, "uhdr_preview_session.json")
	local lines = {
		"{",
		'  "version": 1,',
		'  "work_dir": ' .. string.format("%q", workDir) .. ",",
		'  "result_path": ' .. string.format("%q", resultPath) .. ",",
		'  "dest_dir": ' .. string.format("%q", destDir) .. ",",
		'  "log_path": ' .. string.format("%q", logPath or "") .. ",",
		'  "items": [',
	}

	for i, item in ipairs(items) do
		local comma = (i < #items) and "," or ""
		lines[#lines + 1] = "    {"
		lines[#lines + 1] = '      "id": "' .. jsonEscape(item.id) .. '",'
		lines[#lines + 1] = '      "label": "' .. jsonEscape(item.label or item.id) .. '",'
		lines[#lines + 1] = '      "sdr": ' .. string.format("%q", item.sdr) .. ","
		lines[#lines + 1] = '      "hdr_tiff": ' .. string.format("%q", item.hdr_tiff or "") .. ","
		lines[#lines + 1] = '      "out": ' .. string.format("%q", item.out) .. ","
		lines[#lines + 1] = '      "final_out": ' .. string.format("%q", item.finalOut or item.out) .. ""
		lines[#lines + 1] = "    }" .. comma
	end

	lines[#lines + 1] = "  ]"
	lines[#lines + 1] = "}"

	local body = table.concat(lines, "\n") .. "\n"
	local f = io.open(sessionPath, "w")
	if not f then
		error("Ultra HDR: could not write preview session: " .. tostring(sessionPath))
	end
	f:write(body)
	f:close()
	return sessionPath
end

function PreviewSession.writeBridge(workDir)
	LrFileUtils.createDirectory(workDir)
	local reqDir = LrPathUtils.child(workDir, "hdr_requests")
	LrFileUtils.createDirectory(reqDir)
	local path = LrPathUtils.child(workDir, "bridge.json")
	local f = io.open(path, "w")
	if not f then
		error("Ultra HDR: could not write bridge: " .. tostring(path))
	end
	f:write('{\n  "enabled": true\n}\n')
	f:close()
	return reqDir
end

local function sanitizeId(id)
	id = tostring(id or "item")
	id = string.gsub(id, "[^%w._%-]", "_")
	if id == "" then
		id = "item"
	end
	return id
end

local function atomicWrite(path, body)
	local tmp = path .. ".tmp"
	local f = io.open(tmp, "w")
	if not f then
		return false
	end
	f:write(body)
	f:close()
	if LrFileUtils.exists(path) then
		pcall(function()
			LrFileUtils.delete(path)
		end)
	end
	local moved = pcall(function()
		if LrFileUtils.move then
			LrFileUtils.move(tmp, path)
		else
			LrFileUtils.copy(tmp, path)
			LrFileUtils.delete(tmp)
		end
	end)
	if not moved or not LrFileUtils.exists(path) then
		pcall(function()
			LrFileUtils.copy(tmp, path)
		end)
		pcall(function()
			LrFileUtils.delete(tmp)
		end)
	end
	return LrFileUtils.exists(path)
end

function PreviewSession.requestsDir(workDir)
	return LrPathUtils.child(workDir, "hdr_requests")
end

function PreviewSession.doneHasValidTiff(workDir, id)
	local reqDir = PreviewSession.requestsDir(workDir)
	local donePath = LrPathUtils.child(reqDir, sanitizeId(id) .. ".done")
	if not LrFileUtils.exists(donePath) then
		return false, nil
	end
	local f = io.open(donePath, "r")
	if not f then
		return false, nil
	end
	local text = f:read("*a") or ""
	f:close()
	if not string.find(text, '"ok"%s*:%s*true') then
		return false, nil
	end
	local tiff = string.match(text, '"hdr_tiff"%s*:%s*"([^"]*)"')
	if not tiff or tiff == "" then
		return false, nil
	end
	tiff = jsonUnescape(tiff)
	if not LrFileUtils.exists(tiff) then
		return false, nil
	end
	return true, tiff
end

function PreviewSession.writeDone(workDir, id, ok, hdrTiff, err)
	local reqDir = PreviewSession.requestsDir(workDir)
	LrFileUtils.createDirectory(reqDir)
	local donePath = LrPathUtils.child(reqDir, sanitizeId(id) .. ".done")
	local body
	if ok then
		body = '{\n  "ok": true,\n  "hdr_tiff": "' .. jsonEscape(hdrTiff or "") .. '"\n}\n'
	else
		body = '{\n  "ok": false,\n  "error": "' .. jsonEscape(err or "HDR TIFF render failed") .. '"\n}\n'
	end
	local reqPath = LrPathUtils.child(reqDir, sanitizeId(id) .. ".req")
	pcall(function()
		if LrFileUtils.exists(reqPath) then
			LrFileUtils.delete(reqPath)
		end
	end)
	return atomicWrite(donePath, body)
end

function PreviewSession.listPendingRequestIds(workDir)
	local ids = {}
	local reqDir = PreviewSession.requestsDir(workDir)
	if not LrFileUtils.exists(reqDir) then
		return ids
	end
	local function consider(filePath)
		local leaf = LrPathUtils.leafName(filePath) or ""
		if not string.match(leaf, "%.req$") then
			return
		end
		local id = nil
		local f = io.open(filePath, "r")
		if f then
			local text = f:read("*a") or ""
			f:close()
			id = string.match(text, '"id"%s*:%s*"([^"]+)"')
			if id then
				id = jsonUnescape(id)
			end
		end
		if not id or id == "" then
			id = string.match(leaf, "^(.+)%.req$")
		end
		if id and id ~= "" then
			local donePath = LrPathUtils.child(reqDir, sanitizeId(id) .. ".done")
			if LrFileUtils.exists(donePath) then
				pcall(function()
					LrFileUtils.delete(filePath)
				end)
				return
			end
			ids[#ids + 1] = id
		end
	end
	if LrFileUtils.files then
		pcall(function()
			for filePath in LrFileUtils.files(reqDir) do
				consider(filePath)
			end
		end)
	elseif LrFileUtils.recursiveFiles then
		pcall(function()
			for filePath in LrFileUtils.recursiveFiles(reqDir) do
				consider(filePath)
			end
		end)
	end
	return ids
end

function PreviewSession.readPriorityId(workDir)
	local reqDir = PreviewSession.requestsDir(workDir)
	local path = LrPathUtils.child(reqDir, "priority.txt")
	if not LrFileUtils.exists(path) then
		return nil
	end
	local f = io.open(path, "r")
	if not f then
		return nil
	end
	local text = f:read("*a") or ""
	f:close()
	local id = string.match(text, "^%s*(.-)%s*$")
	if not id or id == "" then
		return nil
	end
	return id
end

function PreviewSession.nextPendingRequestId(workDir)
	local pending = PreviewSession.listPendingRequestIds(workDir)
	if #pending == 0 then
		return nil
	end
	local priority = PreviewSession.readPriorityId(workDir)
	if priority then
		for _, id in ipairs(pending) do
			if id == priority then
				return id
			end
		end
	end
	return pending[1]
end

local function readResultText(resultPath)
	if not resultPath or not LrFileUtils.exists(resultPath) then
		return nil
	end
	local f = io.open(resultPath, "r")
	if not f then
		return nil
	end
	local text = f:read("*a") or ""
	f:close()
	return text
end

function PreviewSession.readApproved(resultPath)
	local text = readResultText(resultPath)
	if not text then
		return false
	end
	return string.find(text, '"approved"%s*:%s*true') ~= nil
end

function PreviewSession.readDestDir(resultPath)
	local text = readResultText(resultPath)
	if not text then
		return nil
	end
	local dest = string.match(text, '"dest_dir"%s*:%s*"([^"]*)"')
	if not dest or dest == "" then
		return nil
	end
	return jsonUnescape(dest)
end

function PreviewSession.readItemStatuses(resultPath)
	local statuses = {}
	local text = readResultText(resultPath)
	if not text then
		return statuses
	end
	for object in string.gmatch(text, "{[^{}]+}") do
		local id = string.match(object, '"id"%s*:%s*"([^"]+)"')
		if id then
			local slices = {}
			local sliceBlock = string.match(object, '"slices"%s*:%s*%[([^%]]*)%]')
			if sliceBlock then
				for slicePath in string.gmatch(sliceBlock, '"([^"]+)"') do
					slices[#slices + 1] = jsonUnescape(slicePath)
				end
			end
			local out = string.match(object, '"out"%s*:%s*"([^"]*)"')
			statuses[id] = {
				skipped = string.find(object, '"skipped"%s*:%s*true') ~= nil,
				encoded = string.find(object, '"encoded"%s*:%s*true') ~= nil,
				slices = slices,
				out = out and jsonUnescape(out) or nil,
			}
		end
	end
	return statuses
end

return PreviewSession
