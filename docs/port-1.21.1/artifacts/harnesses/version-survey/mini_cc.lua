-- mini_cc.lua: just enough of CC:T 1.121.0's bios for the ROM's cc.require to work (runs on Cobalt and LuaJIT).
-- Needs globals ROOT, __readfile(hostpath) -> string|nil, print. ROOT is the survey's scratch workspace
-- (W in ../../../reports/version-survey.md: harness/, libs/, rom-1.121.0/), not this directory. ROOT/rom-1.121.0
-- holds bios.lua and rom/ from the CC:T 1.121.0 core jar. They are CCPL / MPL-2.0, so keep ROOT outside this
-- repository:
--   python -I <repo>/docs/port-1.21.1/artifacts/extract_cct_rom.py <core jar> <ROOT>/rom-1.121.0
local ROOT, readfile = assert(ROOT), assert(__readfile)
local function hostpath(p)
  p = p:gsub("^/+", "")
  if p:sub(1, 4) == "rom/" then return ROOT .. "/rom-1.121.0/" .. p end
  if p:sub(1, 5) == "libs/" then return ROOT .. "/" .. p end
  return nil
end
fs = {}
function fs.combine(a, b)
  local s = (a == "" and b) or (a .. "/" .. b)
  return (s:gsub("/+", "/"):gsub("^/", ""):gsub("/$", ""))
end
function fs.exists(p) local h = hostpath(p) return h ~= nil and readfile(h) ~= nil end
function fs.isDir(p) return false end
-- Like bios.lua's loadfile, this names the chunk "@/" .. fs.combine(path).
function loadfile(path, mode, env)
  local h = hostpath(path)
  local src = h and readfile(h)
  if not src then return nil, "File not found" end
  return load(src, "@/" .. fs.combine(path, ""), mode, env or _G)
end
function dofile(path) local f = assert(loadfile(path, nil, _G)) return f() end
-- CC's rom/apis/io.lua surface (stubs; no popen/tmpfile, like CC)
local function nop() end
io = { stdin = {}, stdout = {}, stderr = {}, close = nop, flush = nop, input = nop, lines = nop, open = nop,
  output = nop, read = nop, type = nop, write = nop }
-- CC's `bit` table over bit32: bios.lua's own `_G.bit = { ... }` statement, cut out of ROOT/rom-1.121.0/bios.lua
-- and run here. Newlines in front keep bios.lua's line numbers in error positions.
do
  local dir = ROOT .. "/rom-1.121.0"
  local changelog, bios = readfile(dir .. "/rom/help/changelog.md"), readfile(dir .. "/bios.lua")
  if not changelog or not bios then error("no CC:T ROM in " .. dir .. ": see the header of mini_cc.lua", 0) end
  if not changelog:find("^# New features in CC: Tweaked 1%.121%.0\r?\n") then
    error("the ROM in " .. dir .. " is not CC:T 1.121.0", 0)
  end
  local first, last = bios:find("\n_G%.bit = %b{}")
  if not first then error("bios.lua in " .. dir .. " has no _G.bit statement", 0) end
  local _, line = bios:sub(1, first):gsub("\n", "")
  assert(load(("\n"):rep(line) .. bios:sub(first + 1, last), "@/bios.lua", "t", _G))()
end
