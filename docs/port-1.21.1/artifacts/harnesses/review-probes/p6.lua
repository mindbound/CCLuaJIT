local function try(name, f) local ok, r = pcall(f); print(string.format("%-36s ok=%s %s", name, tostring(ok), tostring(r):sub(1,70))) end
for _, n in ipairs{8001, 30000, 60000, 64000, 2^19} do
  try("byte(1,-1) n="..n, function() return #{ (" "):rep(n):byte(1,-1) } end)
  try("string.char(unpack) n="..n, function() local t={} for i=1,n do t[i]=65 end return #string.char(unpack(t)) end)
  try("select('#', unpack) in coroutine n="..n, function() local t={} for i=1,n do t[i]=i end
      return coroutine.wrap(function() return select('#', unpack(t)) end)() end)
end
-- JIT-heavy loop with large unpack to exercise recorder
try("jit loop unpack 20000 x200", function() local t={} for i=1,20000 do t[i]=i end local s=0 for k=1,200 do s=s+select('#', unpack(t)) end return s end)
try("deep recursion still bounded", function() local function f(n) return f(n+1)+1 end return f(1) end)
