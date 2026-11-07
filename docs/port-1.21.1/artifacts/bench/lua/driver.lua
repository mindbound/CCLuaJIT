-- ==== driver ====
local iters = ITERS or 5
for it = 1, iters do
  local t0 = CLOCK()
  local r = run()
  local dt = CLOCK() - t0
  OUT(string.format("RESULT\t%s\t%s\t%d\t%.6f\t%s", VMNAME, NAME, it, dt, tostring(r)))
end
