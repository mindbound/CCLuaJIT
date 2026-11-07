local n, m = tonumber(arg[1]), tonumber(arg[2])
local s=("a"):rep(n); local p=("a"):rep(m).."b"
local t=os.clock(); local r = s:find(p,1,true)
print(string.format("plain find n=%g m=%g -> %s in %.2fs (%.3g byte-compares)", n, m, tostring(r), os.clock()-t, (n-m)*m))
t=os.clock(); r = s:find(p)
print(string.format("pattern(no specials) find -> %s in %.2fs", tostring(r), os.clock()-t))
