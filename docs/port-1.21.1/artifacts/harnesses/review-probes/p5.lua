-- privileged trampoline: holds a native function + handle in locals, yields while suspended
local native_resume = function(h, ...) return "PRIVILEGED CALL with handle "..tostring(h) end
local function trampoline(handle)
  local nr, hd = native_resume, handle
  local ev = coroutine.yield("task_complete")
  return nr(hd, ev)
end
local co = coroutine.create(function() return trampoline(42) end)
coroutine.resume(co)
-- attacker in parent coroutine:
for lvl = 0, 3 do
  local info = debug.getinfo(co, lvl, "fS")
  if info then
    print("level", lvl, info.short_src, info.what, info.func)
    for i = 1, 5 do
      local n, v = debug.getlocal(co, lvl, i)
      if n then print("   local", i, n, v) end
    end
  end
end
local f = debug.getinfo(co, 1, "f").func
local _, nr = debug.getlocal(co, 1, 2)
print("stolen call:", pcall(nr, 999))
-- __metatable vs debug.getmetatable
local ud_mt = {__metatable = "locked", __index = {}}
local ud = setmetatable({}, ud_mt)
print("getmetatable:", getmetatable(ud), "debug.getmetatable:", debug.getmetatable(ud) == ud_mt)
