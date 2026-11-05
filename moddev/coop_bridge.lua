-- coop bridge (test instances only): hand LuaSocket and small helpers to the sandboxed coop mod
-- via the shared `foundation` table.
package.cpath = package.cpath .. ";mods/libs/?.dll;mods/libs/?/core.dll"
local ok, socket = pcall(require, "socket.core")
if not ok then ok, socket = pcall(require, "socket") end
local dir = os.getenv("FCOOP_INSTANCE_DIR") or "."
rawset(foundation, "FCOOP", {
  socket = ok and socket or nil,
  instance = dir,
  role = os.getenv("FCOOP_ROLE") or "",
  -- dev only: compile a snippet and run it in the given environment table
  compile = function(src, env)
    local f, err = loadstring(src, "=eval")
    if not f then return nil, err end
    setfenv(f, env)
    return f
  end,
  -- read a file from the instance folder (plain names only, no paths)
  readFile = function(name)
    if type(name) ~= "string" or not name:match("^[%w_%-%.]+$") or name:find("%.%.") then return nil end
    local f = io.open(dir .. "/" .. name, "rb")
    if not f then return nil end
    local s = f:read("*a"); f:close()
    return s
  end,
})
local f = io.open(dir .. "/bridge.txt", "w"); if f then f:write("bridge socket ok=", tostring(ok), "\n"); f:close() end
