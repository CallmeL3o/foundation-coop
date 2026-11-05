-- dev query: list every building (asset, id, position) in the instance whose mod answers on 127.0.0.1:27791
local out = {}
level:getComponentManager("COMP_BUILDING"):getAllComponent():forEach(function(b)
  out[#out + 1] = buildingInfo(b)
end)
return #out .. " buildings", table.concat(out, " || ")
