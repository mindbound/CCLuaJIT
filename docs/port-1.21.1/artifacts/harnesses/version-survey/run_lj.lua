-- run_lj.lua <variant> <root>: emulate the CCLuaJIT sandbox (cclj_prebios.lua) on the patched LuaJIT, apply one
-- identity variant, then run mini_cc.lua + libtests.lua. Variant tokens joined with '+':
--   a         baseline: _VERSION "Lua 5.2", no jit, registry._LOADED = string/table/math/coroutine/debug/bit32/utf8
--   b1/b2/b3  _VERSION "Lua 5.2 (LuaJIT)" / "Lua 5.2 LuaJIT 2.1" / "LuaJIT 2.1 (Lua 5.2)"
--   c         global jit = read-only proxy {version, version_num};  cL: same, also registry._LOADED.jit
--   d         _HOST .. " (CCLuaJIT 0.1.0; LuaJIT 2.1)"
--   bitL      registry._LOADED.bit = LuaJIT's native bit (BitOp) library (not a global)
local variant, root = arg[1], arg[2]
local io_open, native_print, native_bit, native_jit, native_loadfile = io.open, print, bit, jit, loadfile
function __readfile(p) local f = io_open(p, "rb") if not f then return nil end local s = f:read("*a") f:close() return s end
ROOT = root
assert(native_loadfile(root .. "/harness/cclj_prebios.lua"))()
print = native_print -- harness output only; CC's bios defines its own print
_HOST = "ComputerCraft 1.121.0 (Minecraft 1.21.1)"
local loaded = debug.getregistry()._LOADED
local tok = {}
for t in variant:gmatch("[^+]+") do tok[t] = true end
if tok.b1 then _VERSION = "Lua 5.2 (LuaJIT)" end
if tok.b2 then _VERSION = "Lua 5.2 LuaJIT 2.1" end
if tok.b3 then _VERSION = "LuaJIT 2.1 (Lua 5.2)" end
if tok.c or tok.cL then
  local info = { version = native_jit.version, version_num = native_jit.version_num }
  jit = setmetatable({}, { __index = info, __newindex = function() error("jit is read-only", 2) end, __metatable = false })
  if tok.cL then loaded.jit = jit end
end
if tok.d then _HOST = _HOST .. " (CCLuaJIT 0.1.0; LuaJIT 2.1)" end
if tok.bitL then loaded.bit = native_bit end
print("VARIANT " .. variant)
for _, f in ipairs({ "mini_cc.lua", "libtests.lua" }) do
  assert(load(__readfile(root .. "/harness/" .. f), "@" .. f, "t", _G))()
end
