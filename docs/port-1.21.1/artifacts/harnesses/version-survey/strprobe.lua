-- strprobe.lua: evaluate the _VERSION and _HOST parsing expressions found in CC code and vendored libraries
-- (transcribed one-liners; source in the second column) against candidate CCLuaJIT identity strings.
-- Prints one line per expression; "DIFF" marks a result that differs from stock CC:T's.
local VERSIONS = {
  "Lua 5.2",                       -- stock CC:T (Cobalt BaseLib.java:66); option (a)
  "Lua 5.2 (LuaJIT)",              -- option (b) candidates
  "Lua 5.2 LuaJIT 2.1",
  "LuaJIT 2.1 (Lua 5.2)",
  "Lua 5.2 (CCLuaJIT 0.1.0)",
}
local HOSTS = {
  "ComputerCraft 1.121.0 (Minecraft 1.21.1)",                                   -- stock (ServerContext.java:233)
  "ComputerCraft 1.121.0 (Minecraft 1.21.1) (CCLuaJIT 0.1.0; LuaJIT 2.1)",     -- option (d) as proposed
  "ComputerCraft 1.121.0 (Minecraft 1.21.1) (CCLuaJIT 0.1.0)",
  "ComputerCraft 1.121.0 (Minecraft 1.21.1; CCLuaJIT 0.1.0)",
  "ComputerCraft 1.121.0 (Minecraft 1.21.1) CCLuaJIT/0.1.0",
  "ComputerCraft 1.121.0 (Minecraft 1.21.1) (UnBIOS)",                          -- what UnBIOS already produces
}
local V = {
  { "CraftOS-PC bios.lua:37, Basalt, Phoenix, telem, dr-roboto", [[return _VERSION == "Lua 5.1"]] },
  { "sha2.lua:140, Phoenix kernel.lua:7988", [[return _VERSION == "Lua 5.2"]] },
  { "Phoenix 00-init.lua:161", [[return _VERSION < "Lua 5.2"]] },
  { "20 non-CC files", [[return _VERSION >= "Lua 5.2"]] },
  { "9 non-CC files", [[return _VERSION <= "Lua 5.1"]] },
  { "lua-MessagePack MessagePack.lua:13", [[return _VERSION < 'Lua 5.3']] },
  { "non-CC", [[return _VERSION >= "Lua 5.3"]] },
  { "compat53 module.lua:2", [[return _VERSION:sub(-3)]] },
  { "luarocks util.lua:425", [[return _VERSION:sub(5)]] },
  { "common idiom", [[return tonumber(_VERSION:sub(5))]] },
  { "non-CC", [[return _VERSION:match("^Lua 5%.2")]] },
  { "non-CC", [[return _VERSION:match('^Lua 5%.[12]$')]] },
  { "non-CC", [[return _VERSION:match("(%d+%.%d+)")]] },
  { "non-CC", [[return _VERSION:match("Lua (%d%.%d)")]] },
  { "non-CC", [[return _VERSION:match("Lua (%S+)")]] },
  { "non-CC", [[return tonumber(_VERSION:match("%d%.*%d*"))]] },
  { "Teal _tl_compat (inspect.lua:1, RobertBouillon)", [[return tonumber((_VERSION or ''):match('[%d.]*$')) or 0]] },
  { "CCJam-2017 vim.lua:3", [[return _VERSION:find("5.1")]] },
  { "croissant do.lua:342-343", [[return tonumber(_VERSION:match("Lua (%d+)")) .. "/" .. tonumber(_VERSION:match("Lua %d+%.(%d+)"))]] },
  { "non-CC", [[return _VERSION:match("^Lua 5%.([1-5])")]] },
}
local H = {
  { "rawterm.lua:279, Phoenix 00-init.lua:1107", [[return _HOST:match("ComputerCraft 1%.1%d+")]] },
  { "rawterm.lua:281", [[return (_HOST:gsub("(ComputerCraft 1%.)([02-9])", "%10%2"))]] },
  { "rawterm.lua:283", [[return _HOST:match("ComputerCraft ([0-9%.]+)")]] },
  { "Phoenix kernel.lua:365", [[return tonumber(_HOST:match("ComputerCraft 1.(%d+)"))]] },
  { "kwei install.lua:137", [[return tonumber(_HOST:sub(17, 19))]] },
  { "obsi2.lua:2004", [[return _HOST:match("%(.-%)"):sub(2, -2)]] },
  { "obsi2.lua:2005", [[return _HOST:sub(15, 21)]] },
  { "obsi2.lua:2007", [[return _HOST:lower():match("minecraft")]] },
  { "wave.lua:34", [[return _HOST:sub(15, #_HOST) >= "1.80"]] },
  { "qtccv wave.lua:40-44 (2nd word)", [[local m = {} for s in string.gmatch(_HOST, "%S+") do m[#m + 1] = s end return m[2] ]] },
  { "disknet.lua:17", [[return (_HOST:find("CCEmuX") or _HOST:find("CC:Tweaked") or _HOST:find("[(]Minecraft")) ~= nil]] },
  { "clsTurtle.lua:32 (Inksaver)", [[return _HOST:sub(_HOST:find("Minecraft") + 10):sub(1, -2):gsub("^%s*(.-)%s*$", "%1")]] },
  { "clsTurtle.lua:35-42 (minor version)", [[local v = _HOST:sub(_HOST:find("Minecraft") + 10):sub(1, -2):gsub("^%s*(.-)%s*$", "%1")
      local parts = {} for p in (v .. "."):gmatch("(.-)%.") do parts[#parts + 1] = p end
      return tostring(tonumber(parts[1] .. "." .. parts[2])) .. " minor=" .. tostring(tonumber(parts[3])) ]] },
  { "yellowbox.lua:113", [[return _HOST:match("^[^%(]+")]] },
  { "WardenOS neofetch.lua:41", [[return (_HOST:gsub("^ComputerCraft ", "CC: Tweaked "))]] },
  { "Basalt main.lua:458, love.lua:25", [[return _HOST:find("CraftOS%-PC") ~= nil]] },
  { "Kristify init.lua:141", [[return string.find(_HOST, "CraftOS-PC", 1, true) ~= nil]] },
  { "craftos2-rom motd.lua:13", [[return _HOST:match("CraftOS%-PC [%a]* ?(v[%d%.]+)")]] },
  { "UnBIOS unbios.lua:16", [[return _HOST:find("UnBIOS") ~= nil]] },
  { "19PHOBOSS98 DroneBaseClassSP.lua:151", [[return string.find(_HOST, "Minecraft 1.20.1") ~= nil]] },
}
local function run(list, var, values)
  for _, e in ipairs(list) do
    local out, base = {}, nil
    for i, val in ipairs(values) do
      local env = { [var] = val, tonumber = tonumber, tostring = tostring, string = string }
      local f, err = load(e[2], "=expr", "t", env)
      local ok, r
      if f then ok, r = pcall(f) else ok, r = false, err end
      local s = ok and (type(r) == "string" and ("%q"):format(r) or tostring(r)) or ("ERROR(" .. tostring(r):gsub("^.-: ", "") .. ")")
      if i == 1 then base = s end
      out[#out + 1] = (i > 1 and s ~= base) and ("DIFF " .. s) or s
    end
    print(("%-60s | %s"):format(e[2]:gsub("^return ", ""):gsub("%s+", " "):sub(1, 60), e[1]))
    for i, s in ipairs(out) do print(("      [%d] %s"):format(i - 1, s)) end
  end
end
print("== _VERSION candidates:") for i, v in ipairs(VERSIONS) do print(("   [%d] %q"):format(i - 1, v)) end
run(V, "_VERSION", VERSIONS)
print("== _HOST candidates:") for i, v in ipairs(HOSTS) do print(("   [%d] %q"):format(i - 1, v)) end
run(H, "_HOST", HOSTS)
