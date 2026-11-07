-- Overhead benchmark for patches 07 (pattern/sort polling, lj_str_find) and
-- 08 (C stack guard at C->VM re-entry points).
-- usage: luajit patbench.lua [reps]  -> one line per benchmark: name<TAB>seconds (best of reps)
local reps = tonumber((...)) or 5
local clock = os.clock

local text = {}
do
  local words = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "lorem", "ipsum",
    "2026-11-07", "key=value", "(nested (parens) here)", "x=10;", "Steve", "CC:Tweaked"}
  local seed = 12345
  for i = 1, 40000 do
    seed = (seed * 16807) % 2147483647
    text[i] = words[seed % #words + 1]
  end
end
local doc = table.concat(text, " ")  -- ~300 KB
local lines = {}
for i = 1, 2000 do lines[i] = text[i] .. " " .. text[i + 1] .. " 2026-11-07 12:" .. (i % 60) .. ":00 id=" .. i end

local B = {}
local function bench(name, f) B[#B + 1] = {name, f} end

-- Patch 07: pattern matching inner loops.
bench("find_class", function() local n = 0 for i = 1, 200 do local p = 1 while true do local s, e = doc:find("%d+%-%d+", p) if not s then break end n = n + 1 p = e + 1 end end return n end)
bench("gmatch_words", function() local n = 0 for i = 1, 120 do for w in doc:gmatch("%a+") do n = n + #w end end return n end)
bench("gsub_captures", function() local n = 0 for i = 1, 50 do local s, c = doc:gsub("(%w+)=(%w+)", "%2=%1") n = n + c + #s end return n end)
bench("match_lines", function() local n = 0 for r = 1, 1000 do for i = 1, #lines do local y, mo, d = lines[i]:match("(%d+)%-(%d+)%-(%d+)") if y then n = n + 1 end end end return n end)
bench("greedy_dotstar", function() local n = 0 for r = 1, 1500 do for i = 1, #lines do local a = lines[i]:match("^(.*)id=") n = n + #a end end return n end)
bench("lazy_dotminus", function() local n = 0 for r = 1, 3000 do for i = 1, #lines do local a = lines[i]:match("^(.-)%s") n = n + #a end end return n end)
bench("balance_b", function() local n = 0 for i = 1, 300 do for m in doc:gmatch("%b()") do n = n + #m end end return n end)
bench("backtrack_small", function() local n = 0 for i = 1, 8000 do if ("aaaaaaaaaaaa"):find("a*a*a*a*b") then n = n + 1 end end return n end)
bench("gsub_string_repl", function() local n = 0 for i = 1, 250 do local s, c = doc:gsub("o", "0") n = n + c end return n end)
bench("find_plain", function() local n = 0 for i = 1, 3000 do local p = 1 while true do local s = doc:find("CC:Tweaked", p, true) if not s then break end n = n + 1 p = s + 1 end end return n end)
bench("find_plain_long", function() local n = 0 local needle = doc:sub(150001, 150064) for i = 1, 20000 do n = n + doc:find(needle, i % 7 + 1, true) end return n end)
bench("sort_numbers", function() local n = 0 for r = 1, 10 do local t = {} local s = 7 for i = 1, 100000 do s = (s * 16807) % 2147483647 t[i] = s end table.sort(t) n = n + t[1] end return n end)
bench("sort_strings", function() local n = 0 for r = 1, 10 do local t = {} for i = 1, 40000 do t[i] = text[i] .. i end table.sort(t) n = n + #t[1] end return n end)
-- Patch 08: C -> VM re-entries (lua_call from library code, coroutine resume).
bench("gsub_fn_callback", function() local n = 0 for i = 1, 100 do local s, c = doc:gsub("%a+", function(w) return w end) n = n + c end return n end)
bench("sort_cmp_callback", function() local n = 0 for r = 1, 8 do local t = {} local s = 7 for i = 1, 50000 do s = (s * 16807) % 2147483647 t[i] = s end table.sort(t, function(a, b) return a > b end) n = n + t[1] end return n end)
bench("tostring_mm", function() local mt = {__tostring = function() return "x" end} local o = setmetatable({}, mt) local n = 0 for i = 1, 5000000 do n = n + #string.format("%s", o) end return n end)
bench("coroutine_pingpong", function() local co = coroutine.wrap(function() while true do coroutine.yield(1) end end) local n = 0 for i = 1, 6000000 do n = n + co() end return n end)
bench("coroutine_resume", function() local co = coroutine.create(function() while true do coroutine.yield(1) end end) local n = 0 for i = 1, 6000000 do local ok, v = coroutine.resume(co) n = n + v end return n end)
bench("load_small", function() local n = 0 for i = 1, 400000 do n = n + load("return " .. i .. " + (1 * (2 + (3)))")() end return n end)

for _, b in ipairs(B) do
  local best = math.huge
  local r
  for i = 1, reps do
    collectgarbage()
    local t0 = clock()
    r = b[2]()
    local dt = clock() - t0
    if dt < best then best = dt end
  end
  io.write(b[1], "\t", string.format("%.4f", best), "\t", tostring(r), "\n")
end
