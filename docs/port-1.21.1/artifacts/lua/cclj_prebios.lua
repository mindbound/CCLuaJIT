--[[
CCLuaJIT pre-BIOS compatibility shim for CC: Tweaked 1.121.0 (Cobalt 0.9.9 dialect) on LuaJIT 2.1.

Run once in the machine's global environment, after the native side has opened the standard
libraries (base+coroutine, string, table, math, debug, bit) and before bios.lua.

It works on a default LuaJIT build and on a LUAJIT_ENABLE_LUA52COMPAT build (the 5.2 pieces it
re-implements are only installed when missing).

SECURITY NOTE: everything here is plain Lua. User code can reach the native functions captured
as upvalues through debug.getupvalue(), so nothing in this file is a security boundary. Bytecode
loading, debug.sethook, io/os/ffi/jit/package must be removed or patched on the C side.
]]

local _G = _G
local type, select, error, rawget, rawset, rawequal, setmetatable, getmetatable, pairs, ipairs, next, tostring, tonumber, pcall =
  type, select, error, rawget, rawset, rawequal, setmetatable, getmetatable, pairs, ipairs, next, tostring, tonumber, pcall
local s_byte, s_char, s_sub, s_find, s_rep, s_format = string.byte, string.char, string.sub, string.find, string.rep, string.format
local t_concat = table.concat
local floor, ceil, huge, fmod, frexp, ldexp = math.floor, math.ceil, math.huge, math.fmod, math.frexp, math.ldexp
local native_bit = assert(rawget(_G, "bit"), "CCLuaJIT shim needs LuaJIT's bit library")
local tobit, band, bor, bxor, bnot, lshift, rshift, arshift, rol, ror =
  native_bit.tobit, native_bit.band, native_bit.bor, native_bit.bxor, native_bit.bnot,
  native_bit.lshift, native_bit.rshift, native_bit.arshift, native_bit.rol, native_bit.ror
local native_unpack = table.unpack or rawget(_G, "unpack")

local function argerror(n, fname, msg, level)
  error(("bad argument #%d to '%s' (%s)"):format(n, fname, msg), (level or 1) + 2)
end
local function checktype(v, n, fname, want)
  if type(v) ~= want then argerror(n, fname, want .. " expected, got " .. (v == nil and "no value" or type(v)), 1) end
end

-------------------------------------------------------------------------------
-- 1. Version / globals the Cobalt machine does not have
-------------------------------------------------------------------------------
_G._VERSION = "Lua 5.2"
for _, k in ipairs({ "collectgarbage", "gcinfo", "newproxy", "module", "require", "package", "io", "os_native",
  "jit", "ffi", "__inext", "dofile", "loadfile", "print" }) do
  -- dofile/loadfile/print are redefined by bios.lua; the native ones read the *server* filesystem/stdout.
  if k ~= "os_native" then _G[k] = nil end
end

-------------------------------------------------------------------------------
-- 2. load / loadstring: text-only, Cobalt messages, `_ENV` emulation
-------------------------------------------------------------------------------
-- Lua 5.2 code uses `_ENV` as a value (`local fs = _ENV`, `load(s, n, "t", _ENV)`,
-- `setmetatable({}, { __index = _ENV })`). LuaJIT has no _ENV, so we make it an ordinary field of
-- every environment table: _G._ENV = _G and env._ENV = env for every load() with an env.
-- This is what CC 1.7x/1.8x's own bios did on Lua 5.1. It does NOT emulate `local _ENV = t` or
-- assignments to _ENV (those need parser support).
local native_load = load
_G._ENV = _G

local function read_all(reader)
  local parts, n = {}, 0
  while true do
    local piece = reader()
    if piece == nil or piece == "" then break end
    if type(piece) ~= "string" then
      if type(piece) == "number" then piece = tostring(piece)
      else error("reader function must return a string", 0) end
    end
    n = n + 1
    parts[n] = piece
  end
  return t_concat(parts, "", 1, n)
end

local function cc_load(chunk, name, mode, env)
  if type(chunk) == "function" then
    local ok, res = pcall(read_all, chunk)
    if not ok then return nil, res end
    chunk = res
    if name == nil then name = "function" end
  elseif type(chunk) ~= "string" then
    if type(chunk) == "number" then chunk = tostring(chunk)
    else argerror(1, "load", "string expected, got " .. (chunk == nil and "no value" or type(chunk))) end
  end
  if mode ~= nil and type(mode) ~= "string" then argerror(3, "load", "string expected, got " .. type(mode)) end
  if env ~= nil and type(env) ~= "table" then argerror(4, "load", "table expected, got " .. type(env)) end
  mode = mode or "bt"
  if s_byte(chunk, 1) == 27 then
    -- Never hand bytecode to LuaJIT: it has no bytecode verifier (memory-unsafe).
    if s_find(mode, "b", 1, true) and mode == "bt" then return nil, "attempt to load a binary chunk" end
    return nil, "attempt to load a binary chunk (mode is " .. mode .. ")"
  end
  if not s_find(mode, "t", 1, true) then return nil, "attempt to load a text chunk (mode is " .. mode .. ")" end
  local fn, err
  if env ~= nil then
    fn, err = native_load(chunk, name, "t", env)
    if fn and rawget(env, "_ENV") == nil then rawset(env, "_ENV", env) end
  else
    fn, err = native_load(chunk, name, "t")
  end
  return fn, err
end
_G.load = cc_load
_G.loadstring = function(s, name)
  if type(s) ~= "string" and type(s) ~= "number" then
    argerror(1, "loadstring", "string expected, got " .. (s == nil and "no value" or type(s)))
  end
  return cc_load(s, name)
end
-- CC:T has no bytecode format: string.dump always fails there.
string.dump = function() error("unable to dump given function", 2) end

-------------------------------------------------------------------------------
-- 3. Lua 5.2 base/table/coroutine pieces missing without LUA52COMPAT
-------------------------------------------------------------------------------
if not table.pack then
  table.pack = function(...) return { n = select("#", ...), ... } end
end
if not table.unpack then table.unpack = native_unpack end
if not rawget(_G, "rawlen") then
  _G.rawlen = function(v)
    local t = type(v)
    if t ~= "table" and t ~= "string" then argerror(1, "rawlen", "table or string expected") end
    return #v -- without LUA52COMPAT, # never consults __len for tables
  end
end
do -- pairs honouring __pairs (native with LUA52COMPAT; detect at runtime)
  local probe = setmetatable({}, { __pairs = function() return function() end, nil, nil end })
  local native_pairs = pairs
  if native_pairs(probe) == next then
    _G.pairs = function(t)
      local mt = getmetatable(t)
      local mm = type(mt) == "table" and rawget(mt, "__pairs")
      if mm then
        local a, b, c = mm(t)
        return a, b, c
      end
      return native_pairs(t)
    end
  end
end
do -- coroutine.running(): Lua 5.2 returns (co, ismain)
  local running = coroutine.running
  if select("#", running()) < 2 then
    coroutine.running = function()
      local co = running()
      return co, co == nil
    end
  end
end
-- ipairs honouring __index (Cobalt/5.3 semantics); native fast path for plain tables.
do
  local native_ipairs = ipairs
  local function inext_meta(t, i)
    i = i + 1
    local v = t[i]
    if v ~= nil then return i, v end
  end
  _G.ipairs = function(t)
    if getmetatable(t) == nil then return native_ipairs(t) end
    if t == nil then argerror(1, "ipairs", "table expected, got no value") end
    return inext_meta, t, 0
  end
end
-- Cobalt extras / aliases
string.gfind = string.gfind or string.gmatch
math.mod = math.mod or math.fmod
table.create = table.create or function() return {} end

-------------------------------------------------------------------------------
-- 4. math differences
-------------------------------------------------------------------------------
do
  local atan, atan2 = math.atan, math.atan2
  math.atan = function(y, x)
    if x == nil then return atan(y) end
    return atan2(y, x)
  end
  -- LuaJIT's math.log(x, b) is inexact for powers of two (math.log(8, 2) == 2.9999999999999996,
  -- 14 of 2^0..2^60 are off), which breaks the common `colours[math.log(c, 2) + 1]` idiom.
  -- Cobalt computes Math.log(x) / Math.log(b), which is exact for those. Powers of two in base 2
  -- are answered exactly via frexp; everything else uses division like Cobalt.
  local log = math.log
  math.log = function(x, b)
    if b == nil then return log(x) end
    if b == 2 and type(x) == "number" and x > 0 then
      local m, e = frexp(x)
      if m == 0.5 then return e - 1 end
    end
    return log(x) / log(b)
  end
end

-------------------------------------------------------------------------------
-- 5. tonumber: LuaJIT also accepts "inf", "nan" and "infinity" (Cobalt returns nil). Only
--    non-finite results are re-checked, so the common path stays cheap. (LuaJIT's extra
--    acceptance of "0b101" binary strings is left alone: a harmless superset.)
-------------------------------------------------------------------------------
do
  local native_tonumber = tonumber
  _G.tonumber = function(v, base)
    local n = native_tonumber(v, base)
    if (n ~= n or n == huge or n == -huge) and base == nil and type(v) == "string" and s_find(v, "[iInN]") then
      return nil
    end
    return n
  end
end

-------------------------------------------------------------------------------
-- 6. table.sort: Cobalt never raises "invalid order function" and allows yields in the
--    comparator; LuaJIT's C sort does neither. Pure-Lua quicksort (JIT-compiled).
-------------------------------------------------------------------------------
do
  local function lt(a, b) return a < b end
  -- Port of Lua 5.1/5.2 ltablib.c auxsort; where PUC Lua raises "invalid order function for
  -- sorting" the scans are clamped instead, so an inconsistent comparator only yields an
  -- unspecified order (as on Cobalt). Elements are only ever swapped, never lost.
  local function auxsort(t, l, u, cmp)
    while l < u do
      if cmp(t[u], t[l]) then t[l], t[u] = t[u], t[l] end
      if u - l == 1 then break end
      local i = floor((l + u) / 2)
      if cmp(t[i], t[l]) then
        t[i], t[l] = t[l], t[i]
      elseif cmp(t[u], t[i]) then
        t[i], t[u] = t[u], t[i]
      end
      if u - l == 2 then break end
      local P = t[i]
      t[i], t[u - 1] = t[u - 1], t[i]
      local j
      i, j = l, u - 1
      while true do
        i = i + 1
        while cmp(t[i], P) do
          if i >= u then break end
          i = i + 1
        end
        j = j - 1
        while cmp(P, t[j]) do
          if j <= l then break end
          j = j - 1
        end
        if j < i then break end
        t[i], t[j] = t[j], t[i]
      end
      t[u - 1], t[i] = t[i], t[u - 1]
      if i - l < u - i then
        auxsort(t, l, i - 1, cmp)
        l = i + 1
      else
        auxsort(t, i + 1, u, cmp)
        u = i - 1
      end
    end
  end
  table.sort = function(t, cmp)
    checktype(t, 1, "sort", "table")
    if cmp ~= nil and type(cmp) ~= "function" then argerror(2, "sort", "function expected, got " .. type(cmp)) end
    local n = #t
    if n > 1 then auxsort(t, 1, n, cmp or lt) end
  end
end

-------------------------------------------------------------------------------
-- 7. bit32 with Cobalt semantics (truncate toward zero, wrap mod 2^32, unsigned results,
--    Java shift-count behaviour), built on LuaJIT's JIT-compiled bit.* functions.
-------------------------------------------------------------------------------
do
  local TWO32, TWO51 = 4294967296, 2251799813685248
  local function toint(v, n, fname)
    if type(v) ~= "number" then
      local c = tonumber(v)
      if c == nil then argerror(n, fname, "number expected, got " .. (v == nil and "no value" or type(v)), 1) end
      v = c
    end
    if v ~= v then return 0 end
    if v >= 9223372036854775807 then return -1 end      -- (long) saturates to 0x7fff...ffff
    if v <= -9223372036854775808 then return 0 end
    if v >= 0 then v = floor(v) else v = ceil(v) end    -- Java (long) truncates toward zero
    if v >= TWO51 or v <= -TWO51 then v = fmod(v, TWO32) end
    return tobit(v)
  end
  local function u(x) if x < 0 then return x + TWO32 end return x end
  local bit32 = {}
  function bit32.band(...)
    local r = -1
    for i = 1, select("#", ...) do r = band(r, toint((select(i, ...)), i, "band")) end
    return u(r)
  end
  function bit32.bor(...)
    local r = 0
    for i = 1, select("#", ...) do r = bor(r, toint((select(i, ...)), i, "bor")) end
    return u(r)
  end
  function bit32.bxor(...)
    local r = 0
    for i = 1, select("#", ...) do r = bxor(r, toint((select(i, ...)), i, "bxor")) end
    return u(r)
  end
  function bit32.btest(...)
    local r = -1
    for i = 1, select("#", ...) do r = band(r, toint((select(i, ...)), i, "btest")) end
    return r ~= 0
  end
  function bit32.bnot(x) return u(bnot(toint(x, 1, "bnot"))) end
  local function shift(x, disp)
    if disp >= 32 or disp <= -32 then return 0 end
    if disp >= 0 then return lshift(x, disp) end
    return rshift(x, -disp)
  end
  function bit32.lshift(x, d) return u(shift(toint(x, 1, "lshift"), toint(d, 2, "lshift"))) end
  function bit32.rshift(x, d) return u(shift(toint(x, 1, "rshift"), -toint(d, 2, "rshift"))) end
  function bit32.arshift(x, d)
    x, d = toint(x, 1, "arshift"), toint(d, 2, "arshift")
    if d >= 0 then return u(arshift(x, d % 32)) end   -- Java masks shift counts to 5 bits
    return u(lshift(x, (-d) % 32))
  end
  function bit32.lrotate(x, d) return u(rol(toint(x, 1, "lrotate"), toint(d, 2, "lrotate") % 32)) end
  function bit32.rrotate(x, d) return u(ror(toint(x, 1, "rrotate"), toint(d, 2, "rrotate") % 32)) end
  function bit32.extract(n, field, width)
    n, field = toint(n, 1, "extract"), toint(field, 2, "extract")
    width = width == nil and 1 or toint(width, 3, "extract")
    if field < 0 then argerror(2, "extract", "field cannot be negative") end
    if width <= 0 then argerror(3, "extract", "width must be postive") end
    if field + width > 32 then error("trying to access non-existent bits", 2) end
    local mask = width == 32 and -1 or (lshift(1, width) - 1)
    return u(band(rshift(n, field), mask))
  end
  function bit32.replace(n, v, field, width)
    n, v, field = toint(n, 1, "replace"), toint(v, 2, "replace"), toint(field, 3, "replace")
    width = width == nil and 1 or toint(width, 4, "replace")
    if field < 0 then argerror(3, "replace", "field cannot be negative") end
    if width <= 0 then argerror(4, "replace", "width must be postive") end
    if field + width > 32 then error("trying to access non-existent bits", 2) end
    local mask = lshift(width == 32 and -1 or (lshift(1, width) - 1), field)
    return u(bor(band(n, bnot(mask)), band(lshift(v, field), mask)))
  end
  _G.bit32 = bit32
end

-------------------------------------------------------------------------------
-- 8. utf8 library (Cobalt 0.9.9 semantics: max U+10FFFF, strict decoding)
-------------------------------------------------------------------------------
do
  local MAXU = 0x10FFFF
  local utf8 = {}
  utf8.charpattern = "[\0-\127\194-\244][\128-\191]*"
  local function enc(cp)
    if cp < 0x80 then return s_char(cp)
    elseif cp < 0x800 then return s_char(0xC0 + floor(cp / 0x40), 0x80 + cp % 0x40)
    elseif cp < 0x10000 then return s_char(0xE0 + floor(cp / 0x1000), 0x80 + floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40)
    else return s_char(0xF0 + floor(cp / 0x40000), 0x80 + floor(cp / 0x1000) % 0x40, 0x80 + floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40) end
  end
  local function toint(v, n, fname)
    local c = tonumber(v)
    if c == nil then argerror(n, fname, "number expected, got " .. (v == nil and "no value" or type(v)), 1) end
    if c >= 0 then return floor(c) end
    return ceil(c)
  end
  function utf8.char(...)
    local n = select("#", ...)
    local out = {}
    for i = 1, n do
      local cp = toint((select(i, ...)), i, "char")
      if cp < 0 or cp > MAXU then argerror(i, "char", "value out of range") end
      out[i] = enc(cp)
    end
    return t_concat(out, "", 1, n)
  end
  local LIMITS = { [0] = 0xFF, 0x7F, 0x7FF, 0xFFFF }
  -- decode at 1-based byte index i; returns codepoint, length (or nil)
  local function decode(s, i)
    local c = s_byte(s, i)
    if c < 0x80 then return c, 1 end
    local count, res = 0, 0
    while c % 0x80 >= 0x40 do               -- (first & 0x40) != 0
      local cc = s_byte(s, i + count + 1)
      if cc == nil or cc < 0x80 or cc >= 0xC0 then return nil end
      count = count + 1
      res = res * 64 + cc % 64
      c = (c * 2) % 256
    end
    res = res + (c % 128) * 2 ^ (count * 5)
    if count > 3 or res > MAXU or res <= LIMITS[count] then return nil end
    return res, count + 1
  end
  local function posrel(pos, len) if pos >= 0 then return pos end return len + pos + 1 end
  local function iscont(s, i) local b = s_byte(s, i) return b ~= nil and b >= 0x80 and b < 0xC0 end
  function utf8.codepoint(s, i, j)
    checktype(s, 1, "codepoint", "string")
    local len = #s
    i = posrel(i == nil and 1 or toint(i, 2, "codepoint"), len)
    j = posrel(j == nil and i or toint(j, 3, "codepoint"), len)
    if i < 1 then argerror(2, "codepoint", "out of bounds") end
    if j > len then argerror(3, "codepoint", "out of bounds") end
    if i > j then return end
    local out, n = {}, 0
    repeat
      local cp, l = decode(s, i)
      if not cp then error("invalid UTF-8 code", 2) end
      n = n + 1
      out[n] = cp
      i = i + l
    until i > j
    return native_unpack(out, 1, n)
  end
  function utf8.len(s, i, j)
    checktype(s, 1, "len", "string")
    local len = #s
    i = posrel(i == nil and 1 or toint(i, 2, "len"), len)
    j = posrel(j == nil and -1 or toint(j, 3, "len"), len)
    if i < 1 or i > len + 1 then argerror(2, "len", "initial position out of string") end
    if j > len then argerror(3, "len", "final position out of string") end
    local n = 0
    while i <= j do
      local cp, l = decode(s, i)
      if not cp then return nil, i end
      n = n + 1
      i = i + l
    end
    return n
  end
  local function codes_iter(s, i)
    local len = #s
    if i <= 0 then i = 1
    elseif i <= len then
      i = i + 1
      while iscont(s, i) do i = i + 1 end
    else
      i = i + 1
    end
    if i > len then return nil end
    local cp, l = decode(s, i)
    if not cp or iscont(s, i + l) then error("invalid UTF-8 code", 2) end
    return i, cp
  end
  function utf8.codes(s)
    checktype(s, 1, "codes", "string")
    return codes_iter, s, 0
  end
  function utf8.offset(s, n, i)
    checktype(s, 1, "offset", "string")
    n = toint(n, 2, "offset")
    local len = #s
    local pos = (n >= 0) and 1 or len + 1
    pos = posrel(i == nil and pos or toint(i, 3, "offset"), len) - 1  -- 0-based
    if pos < 0 or pos > len then argerror(3, "offset", "position out of bounds") end
    local function cont0(p) return iscont(s, p + 1) end
    if n == 0 then
      while pos > 0 and cont0(pos) do pos = pos - 1 end
    else
      if cont0(pos) then error("initial position is a continuation byte", 2) end
      if n < 0 then
        while n < 0 and pos > 0 do
          repeat pos = pos - 1 until not (pos > 0 and cont0(pos))
          n = n + 1
        end
      else
        n = n - 1
        while n > 0 and pos < len do
          repeat pos = pos + 1 until not cont0(pos)
          n = n - 1
        end
      end
    end
    if n ~= 0 then return nil end
    if cont0(pos) then error("initial position is a continuation byte", 2) end
    local e = pos
    local b = s_byte(s, pos + 1)
    if pos < len and b >= 0x80 then
      while cont0(e + 1) do e = e + 1 end
    end
    return pos + 1, e + 1
  end
  _G.utf8 = utf8
end

-------------------------------------------------------------------------------
-- 9. string.pack / string.unpack / string.packsize (Lua 5.3 formats, Cobalt limits:
--    size_t = 4, default max alignment 1, native = little endian)
-------------------------------------------------------------------------------
do
  local SIZE_T = 4
  local function digit(c) return c and c >= 48 and c <= 57 end
  local function getnum(fmt, pos, default)
    local c = s_byte(fmt, pos)
    if not digit(c) then return default, pos end
    local a = 0
    repeat
      a = a * 10 + (c - 48)
      pos = pos + 1
      c = s_byte(fmt, pos)
    until not digit(c) or a > 214748364
    return a, pos
  end
  local function getnumlimit(fmt, pos, default)
    local sz, np = getnum(fmt, pos, default)
    if sz > 16 or sz <= 0 then error(("integral size (%d) out of limits [1,16]"):format(sz), 4) end
    return sz, np
  end
  -- returns mode, size, newpos
  local function getoption(st, fmt, pos)
    local c = s_sub(fmt, pos, pos)
    pos = pos + 1
    if c == "b" then return "int", 1, pos
    elseif c == "B" then return "uint", 1, pos
    elseif c == "h" then return "int", 2, pos
    elseif c == "H" then return "uint", 2, pos
    elseif c == "l" or c == "j" then return "int", 8, pos
    elseif c == "L" or c == "J" then return "uint", 8, pos
    elseif c == "T" then return "uint", SIZE_T, pos
    elseif c == "f" then return "float", 4, pos
    elseif c == "d" or c == "n" then return "double", 8, pos
    elseif c == "i" then local sz sz, pos = getnumlimit(fmt, pos, 4) return "int", sz, pos
    elseif c == "I" then local sz sz, pos = getnumlimit(fmt, pos, 4) return "uint", sz, pos
    elseif c == "s" then local sz sz, pos = getnumlimit(fmt, pos, SIZE_T) return "string", sz, pos
    elseif c == "c" then
      local sz, np = getnum(fmt, pos, -1)
      if sz < 0 then error("missing size for format option 'c'", 4) end
      return "char", sz, np
    elseif c == "z" then return "zstr", 0, pos
    elseif c == "x" then return "padding", 1, pos
    elseif c == "X" then return "paddalign", 0, pos
    elseif c == " " then return "none", 0, pos
    elseif c == "<" or c == "=" then st.little = true return "none", 0, pos
    elseif c == ">" then st.little = false return "none", 0, pos
    elseif c == "!" then local sz sz, pos = getnumlimit(fmt, pos, 8) st.maxalign = sz return "none", 0, pos
    end
    error("invalid format option '" .. c .. "'", 4)
  end
  local function getdetails(st, fmt, pos, total)
    local mode, size, np = getoption(st, fmt, pos)
    local align = size
    if mode == "paddalign" then
      if np > #fmt then error("invalid next option for option 'X'", 3) end
      -- As in Lua 5.3/Cobalt, the next option is consumed: it only supplies the alignment.
      local m2, s2, np2 = getoption(st, fmt, np)
      if m2 == "char" or s2 == 0 then error("invalid next option for option 'X'", 3) end
      align = s2
      size = 0
      np = np2
    end
    local alignto = 0
    if align > 1 and mode ~= "char" then
      if align > st.maxalign then align = st.maxalign end
      if align > 1 then
        if band(align, align - 1) ~= 0 then error("bad argument #1 to 'pack' (format asks for alignment not power of 2)", 3) end
        alignto = (align - total % align) % align
      end
    end
    return mode, size, np, alignto
  end
  local function trunc(v) if v >= 0 then return floor(v) end return ceil(v) end
  -- Emulates Cobalt: the value goes through Java (long) (saturating), the low min(size, 8) bytes of
  -- its two's complement are written, and bytes 9..size are 0xFF only for negative signed ints.
  local function packint(out, v, size, little, signext)
    local bytes = {}
    local neg = v < 0
    if v >= 9223372036854775807 then                -- Long.MAX_VALUE
      for i = 1, 7 do bytes[i] = 255 end
      bytes[8] = 127
      neg = false
    elseif v <= -9223372036854775808 then           -- Long.MIN_VALUE
      for i = 1, 7 do bytes[i] = 0 end
      bytes[8] = 128
    else
      local a = neg and -v or v                     -- |v| is exact for every double
      for i = 1, 8 do
        local b = a % 256
        a = (a - b) / 256
        bytes[i] = b
      end
      if neg then                                   -- two's complement: invert, then add 1
        local carry = 1
        for i = 1, 8 do
          local b = 255 - bytes[i] + carry
          if b == 256 then b = 0 carry = 1 else carry = 0 end
          bytes[i] = b
        end
      end
    end
    for i = 9, size do bytes[i] = (neg and signext) and 255 or 0 end
    if not little then
      for i = 1, floor(size / 2) do bytes[i], bytes[size + 1 - i] = bytes[size + 1 - i], bytes[i] end
    end
    out[#out + 1] = s_char(native_unpack(bytes, 1, size))
  end
  local function float_bits(x, single)
    -- returns sign, biased exponent, mantissa (integer) for IEEE754 binary32/binary64
    local mbits, ebits = single and 23 or 52, single and 8 or 11
    local bias = single and 127 or 1023
    local emax = 2 ^ ebits - 1
    local sign = 0
    if x < 0 then sign = 1 x = -x end                  -- Cobalt has no -0 (it normalises to 0)
    if x ~= x then return sign, emax, 2 ^ (mbits - 1) end
    if x == huge then return sign, emax, 0 end
    if x == 0 then return sign, 0, 0 end
    local m, e = frexp(x)                              -- x = m * 2^e, 0.5 <= m < 1
    local exp = e - 1 + bias
    local mant
    if exp <= 0 then                                   -- subnormal
      mant = x / 2 ^ (1 - bias - mbits)
      exp = 0
    else
      mant = (m * 2 - 1) * 2 ^ mbits
    end
    -- round half to even
    local fl = floor(mant)
    local frac = mant - fl
    if frac > 0.5 or (frac == 0.5 and fl % 2 == 1) then fl = fl + 1 end
    mant = fl
    if mant >= 2 ^ mbits then
      mant = mant - 2 ^ mbits
      exp = exp + 1
      if exp == 0 then exp = 1 end
    elseif exp == 0 and mant >= 2 ^ mbits then
      exp = 1
    end
    if exp >= emax then return sign, emax, 0 end      -- overflow to infinity
    return sign, exp, mant
  end
  local function packfloat(out, x, size, little)
    local single = size == 4
    local sign, exp, mant = float_bits(x, single)
    local mbits = single and 23 or 52
    local bits = (sign * 2 ^ (single and 8 or 11) + exp) * 2 ^ mbits + mant
    -- bits may exceed 2^53 for doubles: emit bytes from the parts instead
    local bytes = {}
    if single then
      for i = 1, 4 do local b = bits % 256 bytes[i] = b bits = (bits - b) / 256 end
    else
      local lo = mant % 2 ^ 32
      local hi = (sign * 2048 + exp) * 2 ^ 20 + (mant - lo) / 2 ^ 32
      for i = 1, 4 do local b = lo % 256 bytes[i] = b lo = (lo - b) / 256 end
      for i = 5, 8 do local b = hi % 256 bytes[i] = b hi = (hi - b) / 256 end
    end
    if not little then
      for i = 1, floor(size / 2) do bytes[i], bytes[size + 1 - i] = bytes[size + 1 - i], bytes[i] end
    end
    out[#out + 1] = s_char(native_unpack(bytes, 1, size))
  end
  local function checknum(v, n)
    local c = tonumber(v)
    if c == nil then argerror(n, "pack", "number expected, got " .. (v == nil and "no value" or type(v)), 2) end
    return c
  end
  function string.pack(fmt, ...)
    checktype(fmt, 1, "pack", "string")
    local st = { little = true, maxalign = 1 }
    local out, total, arg = {}, 0, 1
    local pos = 1
    while pos <= #fmt do
      local mode, size, np, alignto = getdetails(st, fmt, pos, total)
      pos = np
      if alignto > 0 then out[#out + 1] = s_rep("\0", alignto) total = total + alignto end
      if mode == "int" or mode == "uint" then
        arg = arg + 1
        local v = checknum((select(arg - 1, ...)), arg)
        v = trunc(v)
        if size < 8 then
          local lim = 2 ^ (size * 8 - 1)
          if mode == "int" and (v < -lim or v >= lim) then argerror(arg, "pack", "integer overflow") end
          if mode == "uint" and (v < 0 or v >= lim * 2) then argerror(arg, "pack", "integer overflow") end
        end
        packint(out, v, size, st.little, mode == "int")
      elseif mode == "float" or mode == "double" then
        arg = arg + 1
        packfloat(out, checknum((select(arg - 1, ...)), arg), size, st.little)
      elseif mode == "char" then
        arg = arg + 1
        local s = select(arg - 1, ...)
        if type(s) == "number" then s = tostring(s) end
        checktype(s, arg, "pack", "string")
        if #s > size then argerror(arg, "pack", "string longer than given size") end
        out[#out + 1] = s .. s_rep("\0", size - #s)
      elseif mode == "zstr" then
        arg = arg + 1
        local s = select(arg - 1, ...)
        if type(s) == "number" then s = tostring(s) end
        checktype(s, arg, "pack", "string")
        if s_find(s, "\0", 1, true) then argerror(arg, "pack", "string contains zeros") end
        out[#out + 1] = s .. "\0"
        size = #s + 1
      elseif mode == "string" then
        arg = arg + 1
        local s = select(arg - 1, ...)
        if type(s) == "number" then s = tostring(s) end
        checktype(s, arg, "pack", "string")
        if size < SIZE_T and #s > 2 ^ (size * 8) then argerror(arg, "pack", "string length does not fit in given size") end
        packint(out, #s, size, st.little, false)
        out[#out + 1] = s
        size = size + #s
      elseif mode == "padding" then
        out[#out + 1] = "\0"
      end
      total = total + size
    end
    return t_concat(out)
  end
  function string.packsize(fmt)
    checktype(fmt, 1, "packsize", "string")
    local st = { little = true, maxalign = 1 }
    local total, pos = 0, 1
    while pos <= #fmt do
      local mode, size, np, alignto = getdetails(st, fmt, pos, total)
      pos = np
      total = total + alignto + size
      if mode == "string" or mode == "zstr" then argerror(1, "packsize", "variable-length format") end
    end
    return total
  end
  -- Reads a size-byte integer (size <= 8) at 1-based pos. Splits into two 32-bit halves so the
  -- final double is correctly rounded (like Java's long -> double in Cobalt).
  local function unpackint(s, pos, size, little, signed)
    local function byte_at(i) return s_byte(s, little and (pos + i - 1) or (pos + size - i)) end
    local lo, hi = 0, 0
    for i = (size < 4 and size or 4), 1, -1 do lo = lo * 256 + byte_at(i) end
    for i = size, 5, -1 do hi = hi * 256 + byte_at(i) end
    if signed then
      if size <= 4 then
        local lim = 2 ^ (size * 8 - 1)
        if lo >= lim then lo = lo - 2 * lim end
      else
        local lim = 2 ^ ((size - 4) * 8 - 1)
        if hi >= lim then hi = hi - 2 * lim end
      end
    end
    return hi * 4294967296 + lo
  end
  local function unpackfloat(s, pos, size, little)
    local b = {}
    for i = 1, size do b[i] = s_byte(s, little and (pos + i - 1) or (pos + size - i)) end
    if size == 4 then
      local sign = b[4] >= 128 and -1 or 1
      local exp = (b[4] % 128) * 2 + floor(b[3] / 128)
      local mant = ((b[3] % 128) * 256 + b[2]) * 256 + b[1]
      if exp == 255 then return mant == 0 and sign * huge or (0 / 0) end
      if exp == 0 then return sign * ldexp(mant, -149) end
      return sign * ldexp(mant + 8388608, exp - 150)
    else
      local sign = b[8] >= 128 and -1 or 1
      local exp = (b[8] % 128) * 16 + floor(b[7] / 16)
      local mant = ((((((b[7] % 16) * 256 + b[6]) * 256 + b[5]) * 256 + b[4]) * 256 + b[3]) * 256 + b[2]) * 256 + b[1]
      if exp == 2047 then return mant == 0 and sign * huge or (0 / 0) end
      if exp == 0 then return sign * ldexp(mant, -1074) end
      return sign * ldexp(mant + 4503599627370496, exp - 1075)
    end
  end
  function string.unpack(fmt, s, init)
    checktype(fmt, 1, "unpack", "string")
    if type(s) == "number" then s = tostring(s) end
    checktype(s, 2, "unpack", "string")
    local len = #s
    local pos = init == nil and 1 or trunc(checknum(init, 3))
    if pos < 0 then pos = len + pos + 1 end
    pos = pos - 1                                     -- 0-based
    if pos > len or pos < 0 then argerror(3, "unpack", "initial position out of string") end
    local st = { little = true, maxalign = 1 }
    local out, n = {}, 0
    local fpos = 1
    while fpos <= #fmt do
      local mode, size, np, alignto = getdetails(st, fmt, fpos, pos)
      fpos = np
      if alignto + size + pos > len then argerror(2, "unpack", "data string too short") end
      pos = pos + alignto
      if mode == "int" or mode == "uint" then
        local base = (size > 8 and not st.little) and (pos + size - 8) or pos
        local v = unpackint(s, base + 1, size > 8 and 8 or size, st.little, mode == "int" or size >= 8)
        if size > 8 then
          -- the extra bytes must be pure sign/zero extension
          local ext = (mode == "int" and v < 0) and 255 or 0
          for i = 9, size do
            local b = s_byte(s, st.little and (pos + i) or (pos + size - i + 1))
            if b ~= ext then error(size .. "-byte integer does not fit into Lua Integer", 2) end
          end
        end
        n = n + 1
        out[n] = v
      elseif mode == "float" or mode == "double" then
        n = n + 1
        out[n] = unpackfloat(s, pos + 1, size, st.little)
      elseif mode == "char" then
        n = n + 1
        out[n] = s_sub(s, pos + 1, pos + size)
      elseif mode == "string" then
        local l = unpackint(s, pos + 1, size, st.little, false)
        if size + l + pos > len then argerror(2, "unpack", "data string too short") end
        n = n + 1
        out[n] = s_sub(s, pos + size + 1, pos + size + l)
        pos = pos + l
      elseif mode == "zstr" then
        local e = s_find(s, "\0", pos + 1, true) or (len + 1)
        n = n + 1
        out[n] = s_sub(s, pos + 1, e - 1)
        pos = pos + (e - 1 - pos) + 1
      end
      pos = pos + size
    end
    n = n + 1
    out[n] = pos + 1
    return native_unpack(out, 1, n)
  end
end

-------------------------------------------------------------------------------
-- 10. debug: hooks are global in LuaJIT and the machine's timeout uses one, so user code
--     must not be able to replace/clear it. (A native, chaining implementation is needed
--     for full parity; this shim just refuses.)
-------------------------------------------------------------------------------
if rawget(_G, "debug") then
  debug.sethook = function() error("debug.sethook is not supported by CCLuaJIT", 2) end
  debug.gethook = function() return nil end
  -- (debug.getinfo option 't'/istailcall is rejected by LuaJIT; not wrapped because a Lua
  --  wrapper shifts stack levels, which cc.expect and cc.internal.exception rely on.)
end

-------------------------------------------------------------------------------
-- 11. Registry: make require("...") see what CC:T's registry._LOADED has, nothing more.
-------------------------------------------------------------------------------
if rawget(_G, "debug") and debug.getregistry then
  local loaded = debug.getregistry()._LOADED
  if type(loaded) == "table" then
    local keep = { string = true, table = true, math = true, coroutine = true, debug = true }
    for k in pairs(loaded) do if not keep[k] then loaded[k] = nil end end
    loaded.bit32 = _G.bit32
    loaded.utf8 = _G.utf8
  end
end
