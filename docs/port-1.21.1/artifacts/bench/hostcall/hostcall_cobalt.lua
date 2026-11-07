-- Host-call overhead from Cobalt. `bench` = CC:T ILuaAPI (real LuaMethod/ResultInterpreterFunction path)
-- `raw` (optional) = plain Cobalt LibFunctions registered directly (Cobalt's own fastest path).
local clock, out = bench.clock, bench.print
local N = 10000000
local WARM = 1000000
local S100 = string.rep("a", 100)

local function loop_num(f, n) local x = 0 for i = 1, n do x = f(x) end return x end
local function loop_nop(f, n) local x = 0 for i = 1, n do f(); x = x + 1 end return x end
local function loop_strin(f, n) local x = 0 for i = 1, n do f(S100); x = x + 1 end return x end
local function loop_strout(f, n) local x = 0 for i = 1, n do local s = f(); x = x + #s end return x end
local function loop_blit(f, n) local x = 0 for i = 1, n do f(S100, S100, S100); x = x + 1 end return x end
local LOOPS = { num = loop_num, nop = loop_nop, strin = loop_strin, strout = loop_strout, blit = loop_blit }

local function T(name, shape, f, n)
  n = n or N
  local loop = LOOPS[shape]
  loop(f, WARM)
  loop(f, WARM)
  local t0 = clock()
  local r = loop(f, n)
  local dt = clock() - t0
  out(string.format("HOSTCALL\t%s\t%s\t%d\t%.2f\t%s", MODE, name, n, dt * 1e9 / n, tostring(r)))
end

T("lua_fn(x+1)", "num", function(x) return x + 1 end)
if raw then
  T("raw_cobalt_nop", "nop", raw.nop)
  T("raw_cobalt_num", "num", raw.num)
  T("raw_cobalt_strin(LuaString)", "strin", raw.strin)
  T("raw_cobalt_strout", "strout", raw.strout)
end
if bench.nop then
T("cct_nop", "nop", bench.nop)
T("cct_num(double)", "num", bench.num)
T("cct_strin(Coerced<String>, like term.write)", "strin", bench.strin)
T("cct_strin(ByteBuffer)", "strin", bench.bytesin)
T("cct_strout(String)", "strout", bench.strout)
T("cct_blit3(3x ByteBuffer, like term.blit)", "blit", bench.blit3)
end
