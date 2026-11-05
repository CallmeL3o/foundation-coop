-- coop probe (test only): per-frame tick + action recorder. Streams events over UDP to 127.0.0.1:27790.
local mod = foundation.createMod()
local B = foundation.FCOOP
mod:log("bridge visible: " .. tostring(B ~= nil) .. ", socket: " .. tostring(B and B.socket ~= nil))

local udp
if B and B.socket then udp = B.socket.udp(); udp:settimeout(0) end
local seq = 0
local function send(msg)
  seq = seq + 1
  if udp then udp:sendto(seq .. " " .. msg, "127.0.0.1", 27790) end
end

-- safe readers ------------------------------------------------------------
local function try(f, ...) local ok, v = pcall(f, ...); if ok then return v end; return nil end
local function assetName(a)
  if a == nil then return "nil" end
  return tostring(try(function() return a.Id end) or try(function() return a:getId() end) or try(function() return a.Name end) or a)
end
local function vec(v) if not v then return "?" end; return string.format("%.2f,%.2f,%.2f", v.x, v.y, v.z) end
local function quat(q) if not q then return "?" end; return string.format("%.3f,%.3f,%.3f,%.3f", q.x, q.y, q.z, q.w) end
local function objInfo(go)
  if not go then return "obj=nil" end
  return string.format("id=%s name=%s pos=%s rot=%s",
    tostring(try(go.getId, go)), tostring(try(function() return go.Name end)),
    vec(try(go.getGlobalPosition, go)), quat(try(go.getGlobalOrientation, go)))
end
local function buildingInfo(b)
  if not b then return "building=nil" end
  return string.format("asset=%s %s", assetName(try(function() return b.AssetBuilding end)), objInfo(try(b.getOwner, b)))
end
local function partInfo(p)
  if not p then return "part=nil" end
  local parent = try(function() return p:getOwner():findFirstObjectWithComponentUp("COMP_BUILDING") end)
  return string.format("type=%s placed=%s preview=%s %s parentBuilding=%s",
    tostring(try(function() return p.BuildingPartType end)), tostring(try(p.getIsPlaced, p)), tostring(try(p.getIsPreview, p)),
    objInfo(try(p.getOwner, p)), tostring(parent and try(function() return parent:getOwner():getId() end)))
end

-- transform sync (test): the build UI moves preview parts directly (no player command), so the sender
-- forwards part transforms and the receiver applies them to the building it created for the peer.
-- roles: "a" listens 27793 and sends to 27794, "b" the reverse; old one-way roles "send"/"recv" use 27793
local XF = { queue = {}, applying = false }
do
  local role = B and B.role or ""
  -- lockstep roles (host/client) do not use this: coop.dll carries part transforms inside its placement events
  XF.canSend = (role == "a" or role == "b" or role == "send")
  XF.canRecv = (role == "a" or role == "b" or role == "recv")
  XF.myPort = (role == "b" or role == "client") and 27794 or 27793
  XF.peerPort = (role == "a" or role == "host") and 27794 or 27793
  XF.sameIds = (role == "client")   -- lockstep: object GUIDs are identical on both sides
end
if B and B.socket and (XF.canSend or XF.canRecv) then
  XF.sock = B.socket.udp(); XF.sock:settimeout(0)
  if XF.canRecv then send("XF listen " .. XF.myPort .. " " .. tostring(XF.sock:setsockname("127.0.0.1", XF.myPort))) end
end
local function guidHex(s) return (tostring(s):gsub("-", ""):lower()) end
-- building parts as a Lua array (the engine list may support forEach or ipairs)
local function partsOf(b)
  local lst = try(b.getBuildingPartList, b)
  local arr = {}
  if not lst then return arr end
  pcall(function() lst:forEach(function(p) arr[#arr + 1] = p end) end)
  if #arr == 0 then pcall(function() for _, p in ipairs(lst) do arr[#arr + 1] = p end end) end
  return arr
end
local function dashed(h) return h:sub(1, 8) .. "-" .. h:sub(9, 12) .. "-" .. h:sub(13, 16) .. "-" .. h:sub(17, 20) .. "-" .. h:sub(21, 32) end

function XF.sendPart(b, p)
  if not (XF.sock and XF.canSend) or XF.applying then return end
  local go = try(p.getOwner, p); local bgo = try(b.getOwner, b)
  local pos, rot = go and try(go.getGlobalPosition, go), go and try(go.getGlobalOrientation, go)
  local bid = bgo and try(bgo.getId, bgo)
  if not (pos and rot and bid) then return end
  local idx = 1
  for i, q in ipairs(partsOf(b)) do if q == p then idx = i end end
  local msg = string.format("T %s %d %.4f %.4f %.4f %.6f %.6f %.6f %.6f", tostring(bid), idx, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z, rot.w)
  XF.sock:sendto(msg, "127.0.0.1", XF.peerPort)
  send("XF sent " .. msg)
end

local function lookupLocal(peerId)
  local txt = B.readFile and B.readFile("bindings.txt") or ""
  local want = guidHex(peerId)
  for peer, loc in txt:gmatch("(%x+) (%x+)") do if peer:lower() == want then return dashed(loc:lower()) end end
  if XF.sameIds then return tostring(peerId) end
  return nil
end

function XF.poll(level)
  if not (XF.sock and XF.canRecv) then return end
  for _ = 1, 16 do
    local data = XF.sock:receive()
    if not data then break end
    XF.queue[#XF.queue + 1] = { msg = data, tries = 0 }
  end
  local keep = {}
  for _, q in ipairs(XF.queue) do
    local id, pidx, x, y, z, qx, qy, qz, qw = q.msg:match("^T (%S+) (%d+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+)$")
    local loc = id and lookupLocal(id)
    local bgo = loc and try(level.find, level, loc)
    -- move the same part the sender's UI moved (by index in the building's part list); fall back to the root
    local go = bgo
    local bcomp = bgo and try(bgo.getComponent, bgo, "COMP_BUILDING")
    local parts = bcomp and partsOf(bcomp) or {}
    local part = parts[tonumber(pidx) or 1]
    if part then go = try(part.getOwner, part) or bgo end
    if go then
      local pos, rot = go:getGlobalPosition(), go:getGlobalOrientation()
      pos.x, pos.y, pos.z = tonumber(x), tonumber(y), tonumber(z)
      rot.x, rot.y, rot.z, rot.w = tonumber(qx), tonumber(qy), tonumber(qz), tonumber(qw)
      XF.applying = true
      local ok, err = pcall(go.setGlobalTransform, go, pos, rot)
      XF.applying = false
      send("XF applied peer " .. id .. " -> local " .. loc .. " ok=" .. tostring(ok) .. " " .. tostring(err or "") .. " now " .. vec(try(go.getGlobalPosition, go)))
    else
      q.tries = q.tries + 1
      if q.tries < 600 then keep[#keep + 1] = q else send("XF dropped " .. q.msg .. " (no binding/object, local=" .. tostring(loc) .. ")") end
    end
  end
  XF.queue = keep
end

-- recorder ------------------------------------------------------------------
local REC = {}
local function hookBuilding(owner, b)
  local function on(evName, fn)
    local ev = try(function() return b[evName] end)
    if ev then event.register(owner, ev, fn) end
  end
  on("ON_CONSTRUCTION_STARTED", function() send("EV building.construction_started " .. buildingInfo(b)) end)
  on("ON_BUILDING_COMPLETED", function(_, fin) send("EV building.completed finished=" .. tostring(fin) .. " " .. buildingInfo(b)) end)
  on("ON_BUILDING_PART_BUILT", function(p) send("EV building.part_built " .. partInfo(p)) end)
  on("ON_BUILDING_PART_REMOVED", function(p, isRoot) send("EV building.part_removed root=" .. tostring(isRoot) .. " " .. partInfo(p)) end)
  on("ON_MARKED_FOR_DESTRUCTION", function(_, d) send("EV building.marked_for_destruction " .. tostring(d) .. " " .. buildingInfo(b)) end)
  on("ON_BUILDING_DESTROYED", function() send("EV building.destroyed " .. buildingInfo(b)) end)
  on("ON_PREVIEW_PART_PLACED", function(p, okPlaced) send("EV building.preview_part_placed ok=" .. tostring(okPlaced) .. " " .. partInfo(p)); XF.sendPart(b, p) end)
  on("ON_EDIT_MODE_CHANGED", function(m) send("EV building.edit_mode " .. tostring(m) .. " " .. buildingInfo(b)) end)
end

function REC.start(owner, level)
  local function mgr(t) return try(level.getComponentManager, level, t) end
  local cmB, cmP, cmW = mgr("COMP_BUILDING"), mgr("COMP_BUILDING_PART"), mgr("COMP_WORKPLACE")
  send("REC start buildings=" .. tostring(cmB ~= nil) .. " parts=" .. tostring(cmP ~= nil) .. " workplaces=" .. tostring(cmW ~= nil))
  if cmB then
    event.register(owner, cmB.ON_COMPONENT_INITIALIZED, function(b) send("EV comp_building.initialized " .. buildingInfo(b)); hookBuilding(owner, b) end)
    event.register(owner, cmB.ON_COMPONENT_DESTROYED, function(b) send("EV comp_building.destroyed " .. buildingInfo(b)) end)
    local n = 0
    try(function() cmB:getAllComponent():forEach(function(b) n = n + 1; hookBuilding(owner, b) end) end)
    send("REC existing buildings " .. n)
  end
  if cmP then
    event.register(owner, cmP.ON_COMPONENT_INITIALIZED, function(p) send("EV comp_part.initialized " .. partInfo(p)) end)
    event.register(owner, cmP.ON_COMPONENT_DESTROYED, function(p) send("EV comp_part.destroyed " .. partInfo(p)) end)
  end
  if cmW then
    event.register(owner, cmW.ON_COMPONENT_INITIALIZED, function(w)
      send("EV workplace.initialized " .. objInfo(try(w.getOwner, w)))
      local ev = try(function() return w.ON_ASSIGNED_WORKER_CHANGED end)
      if ev then event.register(owner, ev, function(...) send("EV workplace.worker_changed args=" .. select("#", ...) .. " " .. objInfo(try(w.getOwner, w))) end) end
    end)
  end
end

-- dev eval channel (test only) ------------------------------------------------
local DEV = {}
local function devOpen()
  if not (B and B.socket and B.compile) then return end
  DEV.sock = B.socket.udp(); DEV.sock:settimeout(0)
  local port = (B.role == "b" or B.role == "client" or B.role == "send") and 27792 or 27791
  local ok = DEV.sock:setsockname("127.0.0.1", port)
  send("DEV listen " .. port .. " " .. tostring(ok))
end
local function devPoll(comp)
  if not DEV.sock then return end
  for _ = 1, 4 do
    local data, ip, port = DEV.sock:receivefrom()
    if not data then return end
    local id, src = data:match("^EVAL (%d+)\n(.*)$")
    if id then
      local env = setmetatable({ level = comp:getLevel(), comp = comp, mod = mod, send = send, try = try,
        vec = vec, quat = quat, objInfo = objInfo, buildingInfo = buildingInfo, partInfo = partInfo, assetName = assetName }, { __index = _G })
      local out = {}
      env.print = function(...) local t = {}; for i = 1, select("#", ...) do t[#t + 1] = tostring((select(i, ...))) end; out[#out + 1] = table.concat(t, " ") end
      local f, err = B.compile(src, env)
      local res
      if not f then res = "COMPILE ERROR " .. tostring(err)
      else
        local r = { pcall(f) }
        if r[1] then local t = {}; for i = 2, #r do t[#t + 1] = tostring(r[i]) end; res = "OK " .. table.concat(t, " | ")
        else res = "ERROR " .. tostring(r[2]) end
      end
      DEV.sock:sendto("RES " .. id .. " " .. res .. (#out > 0 and ("\n" .. table.concat(out, "\n")) or ""), ip, port)
    end
  end
end

-- tick component ------------------------------------------------------------
local COMP_COOP_TICK = { TypeName = "COMP_COOP_TICK", ParentType = "COMPONENT", Properties = {} }
function COMP_COOP_TICK:init()
  self._n, self._acc = 0, 0
  self._tag = tostring(try(function() return self:getOwner().Name end))
  send("init on " .. self._tag)
  if self._tag == "Manager" then
    local ok, err = pcall(REC.start, self, self:getLevel())
    if not ok then send("REC start error " .. tostring(err)) end
    devOpen()
  end
end
function COMP_COOP_TICK:update()
  self._n = self._n + 1
  if self._tag == "Manager" then devPoll(self); XF.poll(self:getLevel()) end
  local lvl = self:getLevel()
  self._acc = self._acc + lvl:getUnscaledDeltaTime()
  if self._acc >= 5 then
    self._acc = 0
    local p = self:getOwner():getGlobalPosition()
    send(string.format("tick %s frame %d timescale %.2f pos %s", self._tag, self._n, lvl:getTimeScale(), vec(p)))
  end
end
mod:registerClass(COMP_COOP_TICK)
mod:registerPrefabComponent("PREFAB_GAME_CAMERA", { DataType = "COMP_COOP_TICK" })
mod:registerPrefabComponent("PREFAB_MANAGER", { DataType = "COMP_COOP_TICK" })
