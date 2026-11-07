local N = tonumber(arg[1])
local t = {}
for i=1,N do t[i] = {i} end
collectgarbage("collect")
print(string.format("heap %.0f MB, %d small tables", collectgarbage("count")/1024, N))
local c=os.clock(); collectgarbage("collect"); print(string.format("full GC (all live) %.3fs", os.clock()-c))
local w = setmetatable({}, {__mode="k"}); for i=1,N do w[t[i]] = true end
c=os.clock(); collectgarbage("collect"); print(string.format("full GC with %d-entry weak table %.3fs", N, os.clock()-c))
t = nil
c=os.clock(); collectgarbage("collect"); print(string.format("full GC freeing all %.3fs", os.clock()-c))
local co = {} 
c=os.clock(); for i=1,N/10 do co[i] = coroutine.create(print) end
print(string.format("%d coroutines: %.0f MB, %.3fs", N/10, collectgarbage("count")/1024, os.clock()-c))
