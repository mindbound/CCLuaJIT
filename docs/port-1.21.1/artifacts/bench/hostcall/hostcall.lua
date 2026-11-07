-- Host-call overhead from LuaJIT. Globals: hb (C functions), MODE ("jit"/"joff"), SECTION.
local hb = hb
local now, out = hb.now, hb.out
local N = 10000000
local WARM = 1000000
local REPS = 3
local S100 = string.rep("a", 100)

-- loops are generic over a host function f with one of three shapes
local function loop_num(f, n) local x = 0 for i = 1, n do x = f(x) end return x end
local function loop_nop(f, n) local x = 0 for i = 1, n do f(); x = x + 1 end return x end
local function loop_strin(f, n) local x = 0 for i = 1, n do f(S100); x = x + 1 end return x end
local function loop_strout(f, n) local x = 0 for i = 1, n do local s = f(); x = x + #s end return x end
local function loop_blit(f, n) local x = 0 for i = 1, n do f(S100, S100, S100); x = x + 1 end return x end
local LOOPS = { num = loop_num, nop = loop_nop, strin = loop_strin, strout = loop_strout, blit = loop_blit }

local function T(name, shape, f, n)
  n = n or N
  local loop = LOOPS[shape]
  loop(f, WARM)                -- warm-up: JIT traces + HotSpot compiles the Java callee
  local ts, r = {}, nil
  for rep = 1, REPS do
    local t0 = now()
    r = loop(f, n)
    ts[rep] = (now() - t0) * 1e9 / n
  end
  table.sort(ts)
  out(string.format("HOSTCALL\t%s\t%s\t%d\t%.2f\t%s\tmin=%.2f max=%.2f", MODE, name, n, ts[math.floor((REPS + 1) / 2)], tostring(r), ts[1], ts[REPS]))
end

if SECTION == "jit" or SECTION == "joff" then
  T("empty_loop(x=x+1)", "num", function(x) return x + 1 end)  -- Lua->Lua call baseline (inlined under JIT)
  T("c_nop", "nop", hb.c_nop)
  T("c_num", "num", hb.c_num)
  T("c_strin(100B)", "strin", hb.c_strin)
  T("c_strout(100B)", "strout", hb.c_strout)
  local ok, ffi = pcall(require, "ffi")
  if ok then
    ffi.cdef("double hb_ffi_num(double x);")
    local lib = ffi.load(JNIBENCH_DLL or "jnibench.dll")
    local f = lib.hb_ffi_num
    T("ffi_num", "num", function(x) return f(x) end)
  end
  T("lean_jni_nop", "nop", hb.lean_nop)
  T("lean_jni_num(double)", "num", hb.lean_num)
  T("lean_jni_strin(String via NewString)", "strin", hb.lean_str)
  T("lean_jni_strin(byte[])", "strin", hb.lean_bytes)
  T("lean_jni_strin(direct ByteBuffer)", "strin", hb.lean_buf)
  T("lean_jni_strout(String->GetStringRegion)", "strout", hb.lean_strout)
  T("lean_jni_strout(byte[])", "strout", hb.lean_bytesout)
  T("boxed_jni_nop(CCLuaJIT-style+localframe)", "nop", hb.boxed_nop)
  T("boxed_jni_num(CCLuaJIT-style+localframe)", "num", hb.boxed_num)
  T("boxed_jni_strin(CCLuaJIT-style+localframe)", "strin", hb.boxed_strin)
  T("boxed_jni_strout(CCLuaJIT-style+localframe)", "strout", hb.boxed_strout)
  T("port_jni_nop(boxed+CC:T LuaMethod/ObjectArguments)", "nop", hb.port_nop)
  T("port_jni_num(boxed+CC:T LuaMethod/ObjectArguments)", "num", hb.port_num)
  T("port_jni_strin(boxed+CC:T LuaMethod/ObjectArguments)", "strin", hb.port_strin)
  T("port_jni_strout(boxed+CC:T LuaMethod/ObjectArguments)", "strout", hb.port_strout)
end

if SECTION == "blit" or SECTION == "blitjoff" then
  T("lean_jni_blit3(3x byte[])", "blit", hb.lean_blit3, 5000000)
  T("boxed_jni_blit3(CCLuaJIT-style+localframe)", "blit", hb.boxed_blit3, 5000000)
  T("port_jni_blit3(boxed+CC:T LuaMethod, ByteBuffer args)", "blit", hb.port_blit3, 5000000)
end

if SECTION == "jit" then
  -- Correctness probes of the CCLuaJIT marshalling (strings are bytes in CC; Java side uses chars 0-255)
  local all = {}
  for i = 0, 255 do all[#all + 1] = string.char(i) end
  all = table.concat(all)
  local back = hb.boxed_echo(all)
  local diff = 0
  for i = 1, math.min(#all, #back) do if all:byte(i) ~= back:byte(i) then diff = diff + 1 end end
  out(string.format("PROBE\tboxed echo of bytes 0..255: sent %d bytes, got %d bytes back, %d differing positions; got[129..136]=%q",
    #all, #back, diff, back:sub(129, 136)))
  local b = hb.boxed_bytes256()
  out(string.format("PROBE\tJava byte[256] (0..255) returned via to_lua_value byte[] branch arrives as a %d-byte Lua string", #b))
  local s = hb.boxed_echo("abc\0def")
  out(string.format("PROBE\tboxed echo of 'abc\\0def' (7 bytes) -> %d bytes", #s))
end

if SECTION == "leak" or SECTION == "leakfix" then
  -- CCLuaJIT never frees the local refs it creates per call (args array, Double boxes, result array) until the
  -- native handleEvent frame returns, i.e. until the Lua program yields.
  local f = (SECTION == "leak") and hb.boxed_leaky_num or hb.boxed_num
  local m0 = hb.mem()
  local x = 0
  local t0 = now()
  for step = 1, 5 do
    for i = 1, 1000000 do x = f(x) end
    out(string.format("LEAK\t%s\tafter %dM calls: private bytes %.0f MB (+%.0f MB), elapsed %.2fs", SECTION, step, hb.mem(), hb.mem() - m0, now() - t0))
  end
end
