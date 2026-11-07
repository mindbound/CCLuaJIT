-- usage: luajit compilebench.lua <rom.pack> <reps>: loadstring() every case <reps> times, best of 5 rounds.
local data = assert(io.open(arg[1], "rb")):read("*a")
local reps = tonumber(arg[2]) or 20
local srcs, pos = {}, 1
while pos <= #data do
  local s, e, id, len = data:find("^@@CASE ([^ ]+) (%d+)\n", pos)
  len = tonumber(len); srcs[#srcs + 1] = data:sub(e + 1, e + len); pos = e + len + 2
end
local best = math.huge
for round = 1, 5 do
  local t0 = os.clock()
  for r = 1, reps do for i = 1, #srcs do assert(loadstring(srcs[i], "=x")) end end
  local dt = os.clock() - t0
  if dt < best then best = dt end
end
print(string.format("%d files x %d reps: best of 5 = %.3f s (%.2f ms per full ROM compile)", #srcs, reps, best, best * 1000 / reps))
