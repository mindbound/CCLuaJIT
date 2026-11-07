-- Differential parse driver.
-- usage: luajit parsediff.lua <pack-file> <out-file> [dump-dir]
-- <pack-file> holds records "@@CASE <id> <nbytes>\n<nbytes bytes>\n".
-- For each case: loadstring(src, "=" .. id). Writes one line per case:
--   <id>\tOK\t<dump length>\t<FNV-1a 32 of string.dump(f)>
--   <id>\tERR\t<error message, with \\ \n \r \t escaped>
-- If [dump-dir] is given, also writes <dump-dir>/<n>.bc for each OK case (n = case index).
local bit = require("bit")
local bxor, band, tobit = bit.bxor, bit.band, bit.tobit
local packfile, outfile, dumpdir = arg[1], arg[2], arg[3]

local function fnv1a(s)
  local h = tobit(0x811c9dc5)
  for i = 1, #s do
    h = bxor(h, s:byte(i))
    -- h * 16777619 mod 2^32, split to stay exact in doubles
    h = tobit(band(h, 0xffff) * 16777619 + (band(bit.rshift(h, 16), 0xffff) * 16777619 % 65536) * 65536)
  end
  return bit.tohex(h)
end

local function esc(s)
  return (s:gsub("[\\\n\r\t]", { ["\\"] = "\\\\", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" }))
end

local fh = assert(io.open(packfile, "rb"))
local data = fh:read("*a")
fh:close()
local out = assert(io.open(outfile, "wb"))
local pos, n = 1, 0
while pos <= #data do
  local s, e, id, len = data:find("^@@CASE ([^ ]+) (%d+)\n", pos)
  assert(s, "bad pack record at byte " .. pos)
  len = tonumber(len)
  local src = data:sub(e + 1, e + len)
  pos = e + len + 2
  n = n + 1
  local ok, f, err = pcall(loadstring, src, "=" .. id)
  if not ok then
    out:write(id, "\tTHROW\t", esc(tostring(f)), "\n")
  elseif f then
    local d = string.dump(f, "d")  -- deterministic: table templates are written in a fixed key order
    out:write(id, "\tOK\t", #d, "\t", fnv1a(d), "\n")
    if dumpdir then
      local df = assert(io.open(dumpdir .. "/" .. n .. ".bc", "wb"))
      df:write(d)
      df:close()
    end
  else
    out:write(id, "\tERR\t", esc(tostring(err)), "\n")
  end
end
out:write("@@END\t", n, "\n")
out:close()
print("cases: " .. n)
