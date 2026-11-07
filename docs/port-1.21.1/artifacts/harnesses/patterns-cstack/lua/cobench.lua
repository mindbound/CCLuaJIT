local reps = tonumber((...)) or 5
local function best(f) local b = math.huge for i = 1, reps do local t0 = os.clock() f() local dt = os.clock() - t0 if dt < b then b = dt end end return b end
io.write("coroutine_pingpong\t", string.format("%.4f", best(function() local co = coroutine.wrap(function() while true do coroutine.yield(1) end end) local n = 0 for i = 1, 6000000 do n = n + co() end return n end)), "\t0\n")
io.write("coroutine_resume\t", string.format("%.4f", best(function() local co = coroutine.create(function() while true do coroutine.yield(1) end end) local n = 0 for i = 1, 6000000 do local ok, v = coroutine.resume(co) n = n + v end return n end)), "\t0\n")
io.write("gsub_fn_callback\t", string.format("%.4f", best(function() local s = ("abc def "):rep(50000) local n = 0 for i = 1, 10 do local r, c = s:gsub("%a+", function(w) return w end) n = n + c end return n end)), "\t0\n")
io.write("host_like_sort_cmp\t", string.format("%.4f", best(function() local t = {} local s = 7 for i = 1, 200000 do s = (s * 16807) % 2147483647 t[i] = s end table.sort(t, function(a, b) return a > b end) end)), "\t0\n")
