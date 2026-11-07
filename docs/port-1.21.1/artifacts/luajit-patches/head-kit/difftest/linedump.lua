-- usage: luajit linedump.lua <file>: per-proto line info via jit.util
local ju = require("jit.util")
local src = assert(io.open(arg[1], "rb")):read("*a")
local f = assert(loadstring(src, "=x"))
local function walk(fn, depth)
  local fi = ju.funcinfo(fn)
  print(string.rep("  ", depth) .. string.format("proto linedefined=%s lastlinedefined=%s params=%s bytecodes=%s",
    fi.linedefined, fi.lastlinedefined, fi.params, fi.bytecodes))
  for pc = 1, fi.bytecodes - 1 do
    local ins = ju.funcbc(fn, pc)
    local li = ju.funcinfo(fn, pc)
    print(string.rep("  ", depth) .. string.format("  pc=%d ins=%08x line=%s", pc, ins, li.currentline))
  end
  for k = -1, -100, -1 do
    local c = ju.funck(fn, k)
    if c == nil then break end
    if type(c) == "proto" then walk(c, depth + 1) end
  end
end
walk(f, 0)
