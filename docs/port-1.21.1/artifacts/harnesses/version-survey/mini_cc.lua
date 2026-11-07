-- mini_cc.lua: just enough of CC:T 1.121.0's bios for the ROM's cc.require to work (runs on Cobalt and LuaJIT).
-- Needs globals ROOT (host path of the version-survey dir), __readfile(hostpath) -> string|nil, print.
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
-- bios.lua: loadfile(filename, mode, env) = load(file.readAll(), "@/" .. fs.combine(filename), mode, env)
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
-- bios.lua lines 21-30 (verbatim): inject a stub for the old bit library
_G.bit = {
    bnot = bit32.bnot,
    band = bit32.band,
    bor = bit32.bor,
    bxor = bit32.bxor,
    brshift = bit32.arshift,
    blshift = bit32.lshift,
    blogic_rshift = bit32.rshift,
}
