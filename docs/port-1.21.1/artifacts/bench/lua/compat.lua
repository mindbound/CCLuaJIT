-- ==== compat prologue (Cobalt/CC:T 1.121.0, LuaJIT 2.1, PUC Lua 5.4) ====
-- Clock/output: on the CC:T harness a host API "bench" provides clock()/print().
local CLOCK, OUT
if bench and bench.clock then
  CLOCK, OUT = bench.clock, bench.print
else
  CLOCK, OUT = os.clock, print
end
local unpack = table.unpack or unpack
local loadstr = loadstring or load
local VMNAME = (jit and (jit.status() and "luajit-jit" or "luajit-joff")) or (bench and "cobalt") or _VERSION

if not bit32 then
  if bit then
    -- LuaJIT. BIT32_MODE="signed" (default) mirrors what CCLuaJIT 1.20.1 does: bit32 = bit
    -- (signed results; note CCLuaJIT does not even alias rrotate/lrotate, we add them).
    -- BIT32_MODE="unsigned" is a correct bit32 shim (unsigned results) for comparison.
    local b = bit
    if BIT32_MODE == "unsigned" then
      local M = 4294967296
      local band, bor, bxor, bnot, lsh, rsh, ars, rol, ror = b.band, b.bor, b.bxor, b.bnot, b.lshift, b.rshift, b.arshift, b.rol, b.ror
      bit32 = {
        band = function(x, y, z) if z then return band(x, y, z) % M end return band(x, y) % M end,
        bor = function(x, y, z) if z then return bor(x, y, z) % M end return bor(x, y) % M end,
        bxor = function(x, y, z) if z then return bxor(x, y, z) % M end return bxor(x, y) % M end,
        bnot = function(x) return bnot(x) % M end,
        lshift = function(x, n) if n >= 32 then return 0 end return lsh(x, n) % M end,
        rshift = function(x, n) if n >= 32 then return 0 end return rsh(x, n) % M end,
        arshift = function(x, n) return ars(x, n) % M end,
        lrotate = function(x, n) return rol(x, n) % M end,
        rrotate = function(x, n) return ror(x, n) % M end,
      }
    else
      bit32 = { band = b.band, bor = b.bor, bxor = b.bxor, bnot = b.bnot, lshift = b.lshift,
        rshift = b.rshift, arshift = b.arshift, lrotate = b.rol, rrotate = b.ror }
    end
  else
    -- PUC Lua 5.3/5.4: build bit32 from native operators inside a string so 5.1 parsers never see them.
    bit32 = loadstr([[
      local M = 0xFFFFFFFF
      local function band(a, b, c) if c then return a & b & c & M end return a & b & M end
      local function bor(a, b, c) if c then return (a | b | c) & M end return (a | b) & M end
      local function bxor(a, b, c) if c then return (a ~ b ~ c) & M end return (a ~ b) & M end
      local function bnot(a) return (~a) & M end
      local function lshift(a, n) if n >= 32 then return 0 end return (a << n) & M end
      local function rshift(a, n) if n >= 32 then return 0 end return (a & M) >> n end
      local function rrotate(a, n) a = a & M; n = n % 32; return ((a >> n) | (a << (32 - n))) & M end
      local function lrotate(a, n) a = a & M; n = n % 32; return ((a << n) | (a >> (32 - n))) & M end
      local function arshift(a, n) a = a & M; if a >= 0x80000000 then return ((a >> n) | ~(M >> n)) & M end return a >> n end
      return { band = band, bor = bor, bxor = bxor, bnot = bnot, lshift = lshift, rshift = rshift,
               rrotate = rrotate, lrotate = lrotate, arshift = arshift }
    ]])()
  end
end
-- ==== end compat prologue ====
