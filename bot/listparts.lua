-- dev query: list every building part (type, placed, position, parent building)
local out = {}
level:getComponentManager("COMP_BUILDING_PART"):getAllComponent():forEach(function(p)
  out[#out + 1] = partInfo(p)
end)
return #out .. " parts", table.concat(out, " || ")
