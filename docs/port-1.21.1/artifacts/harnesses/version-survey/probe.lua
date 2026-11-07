-- probe.lua: evaluate the version/host/LuaJIT detection expressions that commonly vendored libraries and CC
-- programs use, under each candidate CCLuaJIT identity. Only these one-line expressions (quoted from the cited
-- files) are evaluated here; the libraries themselves are not run.
-- Globals in: VARIANTS (list of names), VMNAME. Works on Cobalt (Lua 5.2 dialect) and LuaJIT.
local NATIVE_JIT = rawget(_G, "jit")
local NATIVE_BIT = rawget(_G, "bit")
local BIT32 = rawget(_G, "bit32") or { extract = function() end, bxor = function() end, band = function() end }
local CCBIT = { bnot = BIT32.bnot, band = BIT32.band, bor = BIT32.bor, bxor = BIT32.bxor, brshift = BIT32.arshift,
  blshift = BIT32.lshift, blogic_rshift = BIT32.rshift }
local HOST = "ComputerCraft 1.121.0 (Minecraft 1.21.1)"
local function minijit()
  local info = { version = NATIVE_JIT and NATIVE_JIT.version or "LuaJIT 2.1.1783773675",
    version_num = NATIVE_JIT and NATIVE_JIT.version_num or 20199 }
  return setmetatable({}, { __index = info, __newindex = function() error("jit is read-only", 2) end, __metatable = false })
end
local DEF = {
  stock = { version = "Lua 5.2", host = HOST },
  a = { version = "Lua 5.2", host = HOST },
  b1 = { version = "Lua 5.2 (LuaJIT)", host = HOST },
  b2 = { version = "Lua 5.2 LuaJIT 2.1", host = HOST },
  b3 = { version = "LuaJIT 2.1 (Lua 5.2)", host = HOST },
  c = { version = "Lua 5.2", host = HOST, jit = minijit },
  cL = { version = "Lua 5.2", host = HOST, jit = minijit, loaded_jit = true },
  d = { version = "Lua 5.2", host = HOST .. " (CCLuaJIT 0.1.0; LuaJIT 2.1)" },
  ["a+bitL"] = { version = "Lua 5.2", host = HOST, loaded_bit = true },
  ["cL+bitL"] = { version = "Lua 5.2", host = HOST, jit = minijit, loaded_jit = true, loaded_bit = true },
  -- CraftOS-PC Accelerated v2.8.3-luajit as built from craftos2@luajit (Computer.cpp:213-222, 435-437), for reference
  accel = { version = "Lua 5.1", host = "ComputerCraft 1.112.0 (CraftOS-PC Accelerated v2.8.3-luajit)",
    jit = function() return NATIVE_JIT end, loaded_jit = true },
}
local function mkenv(v)
  local jitv = v.jit and v.jit() or nil
  local LOADED = { string = string, table = table, math = math, coroutine = coroutine, debug = debug, bit32 = BIT32 }
  if v.loaded_jit then LOADED.jit = jitv end
  if v.loaded_bit then LOADED.bit = NATIVE_BIT end
  local env = { _VERSION = v.version, _HOST = v.host, jit = jitv, bit = CCBIT, bit32 = BIT32, string = string,
    table = table, math = math, type = type, tostring = tostring, tonumber = tonumber, pcall = pcall, assert = assert,
    ipairs = ipairs, select = select, rawget = rawget, setmetatable = setmetatable, error = error }
  env._G = env
  env.require = function(name)
    local m = LOADED[name]
    if m ~= nil then return m end
    error("module '" .. name .. "' not found", 2)
  end
  return env
end
local CHECKS = {
  { "sha2: is_LuaJIT", "pure_lua_SHA sha2.lua:120",
    [[return ({false, [1] = true})[1] and _VERSION ~= "Luau" and (type(jit) ~= "table" or jit.version_num >= 20000)]] },
  { "sha2: bit library", "sha2.lua:120-146", [[
    local is_LuaJIT = ({false, [1] = true})[1] and _VERSION ~= "Luau" and (type(jit) ~= "table" or jit.version_num >= 20000)
    if is_LuaJIT then
      local ok, b = pcall(require, "bit")
      if not ok then return "LuaJIT branch; require'bit' errors -> sha2 fails to load" end
      return "LuaJIT branch; require'bit' ok (ror=" .. type(b.ror) .. ")"
    end
    for _, libname in ipairs(_VERSION == "Lua 5.2" and {"bit32", "bit"} or {"bit", "bit32"}) do
      if type(_G[libname]) == "table" and _G[libname].bxor then
        return "vanilla branch; _G." .. libname .. (libname == "bit" and " = CC stub (lshift=" .. type(_G.bit.lshift) .. ")" or "")
      end
    end
    return "vanilla branch; none" ]] },
  { "MessagePack: header probe", "lua-MessagePack src/MessagePack.lua:5-17", [[
    local r, jit = pcall(require, 'jit'); if not r then jit = nil end
    return (not jit and _VERSION < 'Lua 5.3') and "string.dump path (CC: errors)" or "skips string.dump" ]] },
  { "Teal/inspect: version num", "inspect.lua:1 (Teal _tl_compat)", [[
    local n = tonumber((_VERSION or ''):match('[%d.]*$')) or 0
    return n .. (n < 5.3 and " (<5.3: try compat53)" or " (>=5.3)") ]] },
  { "compat53: lua_version", "compat53/module.lua:2,7,420", [[
    local v = _VERSION:sub(-3)
    return ("%q <5.3=%s ==5.1=%s"):format(v, tostring(v < "5.3"), tostring(v == "5.1")) ]] },
  { "Penlight: lua51 / jit", "pl/compat.lua:14,18", [[
    return ("lua51=%s jit=%s"):format(tostring(_VERSION == 'Lua 5.1'), tostring(tostring(assert):match('builtin') ~= nil)) ]] },
  { "lbase64: extract impl", "lbase64 base64.lua:28-34", [[
    if _G.bit32 and _G.bit32.extract then return "bit32.extract"
    elseif _G.bit then return "_G.bit (lshift=" .. type(_G.bit.lshift) .. ")"
    elseif _G._VERSION == "Lua 5.1" then return "5.1 arithmetic" else return "5.3 operators" end ]] },
  { "AUKit: sinc window/chunk", "aukit.lua:131,1011", [[return (jit and 30 or 10) .. " / " .. (jit and 7680 or 32768)]] },
  { "CraftOS-PC bios: 5.1 layer", "craftos2-rom bios.lua:14,37", [[
    return _VERSION == "Lua 5.1" and "loadstring + 5.1 compat layer" or "5.2 path" ]] },
  { "CraftOS-PC bios: jit.off", "craftos2-rom bios.lua:23", [[
    if jit and jit.os == "OSX" and jit.arch == "arm64" then return "calls jit.off()" end return "no call" ]] },
  { "CraftOS-PC motd: version", "craftos2-rom motd.lua:13", [[return tostring(_HOST:match("CraftOS%-PC [%a]* ?(v[%d%.]+)"))]] },
  { "Basalt: CraftOS-PC check", "Basalt/main.lua:458", [[return tostring(_HOST:find("CraftOS%-PC") ~= nil)]] },
  { "AUKit: CCPC 2.6.4 check", "aukit.lua:2000", [[return tostring(_HOST:find("CraftOS-PC v2.6.4") ~= nil)]] },
  { "rxi json bench: VM name", "json.lua bench/util/bench.lua:43", [[return jit and jit.version or _VERSION]] },
  { "VM: tostring(assert)", "(VM)", [[return (tostring(assert):gsub("%x%x%x%x%x%x+$", "<hex>"))]] },
}
for _, name in ipairs(VARIANTS) do
  local v = assert(DEF[name], name)
  for _, c in ipairs(CHECKS) do
    local env = mkenv(v)
    local f, err = load(c[3], "=" .. c[1], "t", env)
    local ok, res
    if f then ok, res = pcall(f) else ok, res = false, err end
    print(("%s\t%s\t%s\t%s\t%s"):format(VMNAME, name, c[1], c[2], ok and tostring(res) or ("ERROR: " .. tostring(res))))
  end
end
