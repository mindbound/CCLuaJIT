-- McIlroy-style adversary ("A Killer Adversary for Quicksort") adapted to the
-- 2-way comparator of LuaJIT's table.sort (Lua 5.1 auxsort). Produces a plain
-- number array that makes the default-comparator sort quadratic.
local function killer(n)
  local val, gas, nsolid, candidate = {}, n + 1, 0, nil
  local ptr = {}
  for i = 1, n do ptr[i] = i; val[i] = gas end
  local function cmp(x, y)
    if val[x] == gas and val[y] == gas then
      nsolid = nsolid + 1
      if x == candidate then val[x] = nsolid else val[y] = nsolid end
    end
    if val[x] == gas then candidate = x elseif val[y] == gas then candidate = y end
    return val[x] < val[y]
  end
  table.sort(ptr, cmp)
  local a = {}
  for i = 1, n do
    if val[i] == gas then nsolid = nsolid + 1; val[i] = nsolid end
    a[i] = val[i]
  end
  return a
end

if ... == "module" then return killer end

local n = tonumber((...)) or 20000
local t00 = os.clock()
local a = killer(n)
local tg = os.clock() - t00
local b = {} for i = 1, n do b[i] = (i * 7919) % n end
local t0 = os.clock() table.sort(b) local t1 = os.clock()
table.sort(a) local t2 = os.clock()
for i = 2, n do assert(a[i-1] <= a[i]) end
print(string.format("n=%d gen %.3f s, sort random %.4f s, sort killer %.3f s", n, tg, t1 - t0, t2 - t1))
