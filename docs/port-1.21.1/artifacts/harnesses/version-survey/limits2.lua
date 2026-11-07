local unpack = table.unpack or unpack
local function ok_byte(n) return (pcall(function() return #{string.rep("a", n):byte(1, -1)} end)) end
local function ok_unpack(n) local t = {} for i = 1, n do t[i] = i end return (pcall(function() return #{unpack(t)} end)) end
local function edge(f) local lo, hi = 1, 1048576 while lo < hi do local mid = math.floor((lo + hi + 1) / 2) if f(mid) then lo = mid else hi = mid - 1 end end return lo end
print("max string.byte results:", edge(ok_byte), "max unpack results:", edge(ok_unpack))
print("string.char(unpack) 7680:", pcall(function() local t = {string.rep("a", 7680):byte(1, -1)} return #string.char(unpack(t)) end))
