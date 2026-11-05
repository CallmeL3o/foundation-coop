-- dev query: a fingerprint of the simulation state to compare two lockstep instances.
-- First line: summary; then a hash over every villager (id, position, behaviour state, job) and every building part
-- (id, position, placed/built) so any drift changes it.
local main = level:getComponentManager("COMP_MAIN_GAME_LOOP")
local loop = main and main:getFirst()
local day = loop and try(loop.getDay, loop) or -1
local function h(acc, s) for i = 1, #s do acc = (acc * 31 + s:byte(i)) % 4294967291 end return acc end
local rows, n, sx, sz = {}, 0, 0, 0
local vm = level:getComponentManager("COMP_VILLAGER")
if vm then vm:getAllComponent():forEach(function(v)
  local go = v:getOwner(); local p = go:getGlobalPosition()
  n = n + 1; sx = sx + p.x; sz = sz + p.z
  local job = try(v.getAssetJob, v)
  rows[#rows + 1] = string.format("V %s %.3f,%.3f,%.3f s%s j%s", tostring(go:getId()), p.x, p.y, p.z,
    tostring(try(v.getVillagerBehaviorState, v)), job and tostring(job):sub(1, 40) or "-")
end) end
local nb, np, built = 0, 0, 0
level:getComponentManager("COMP_BUILDING"):getAllComponent():forEach(function(b)
  nb = nb + 1
  rows[#rows + 1] = string.format("B %s e%s", tostring(b:getOwner():getId()), tostring(try(b.getEditMode, b)))
end)
level:getComponentManager("COMP_BUILDING_PART"):getAllComponent():forEach(function(p)
  np = np + 1
  local go = p:getOwner(); local q = go:getGlobalPosition()
  local isb = try(p.getIsBuilt, p); if isb then built = built + 1 end
  rows[#rows + 1] = string.format("P %s %.3f,%.3f,%.3f p%s b%s", tostring(go:getId()), q.x, q.y, q.z,
    tostring(try(p.getIsPlaced, p)), tostring(isb))
end)
table.sort(rows)
local acc = 7
for _, r in ipairs(rows) do acc = h(acc, r) end
return string.format("day %s villagers %d sum %.3f,%.3f buildings %d parts %d built %d hash %08x",
  tostring(day), n, sx, sz, nb, np, built, acc)
