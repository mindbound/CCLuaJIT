-- usage: luajit genkiller.lua <dir-of-killer.lua> <n> <outfile>
local dir, n, out = ...
local killer = assert(loadfile(dir .. "/killer.lua"))("module")
local a = killer(tonumber(n))
local f = assert(io.open(out, "w"))
for i = 1, #a do f:write(a[i], "\n") end
f:close()
