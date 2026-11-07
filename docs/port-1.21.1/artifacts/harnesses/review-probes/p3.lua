local function bench(name, src)
  local t=os.clock(); local f, e = loadstring(src)
  print(string.format("%-34s %6.1f MB  load %.2fs  %s", name, #src/2^20, os.clock()-t, f and "ok" or tostring(e):sub(1,60)))
end
local N = tonumber(arg[1]) or 2e6
local b = {}
for i=1,N do b[i]=tostring(i) end
bench("table ctor of numbers", "return {"..table.concat(b, ",").."}")
for i=1,N do b[i]='"s'..i..'"' end
bench("table ctor of unique strings", "return {"..table.concat(b, ",").."}")
for i=1,N do b[i]='x=x+'..i end
bench("straight-line statements", "local x=0 "..table.concat(b, " ").." return x")
for i=1,N do b[i]='["k'..i..'"]='..i end
bench("hash ctor", "return {"..table.concat(b, ",").."}")
local s = ("--"..("x"):rep(1000).."\n"):rep(N/50)
bench("comments only", s)
