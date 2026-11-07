-- Behavioural checks for upstream fixes picked onto the backport branch.
-- Run as: luajit -Ohotloop=1 fixcheck.lua   (so tiny loops get compiled)
local results = {}
local function check(name, fn)
  local ok, res, detail = pcall(fn)
  if not ok then res, detail = false, "error: " .. tostring(res) end
  print(string.format("%-4s %-52s %s", res and "PASS" or "FAIL", name, detail or ""))
end

-- 95f931cb (#1534): ALEN must not be CSE'd/forwarded across a NEWREF that rehashes the table.
check("95f931cb #t after NEWREF rehash (CSE case)", function()
  local res = 0
  for _ = 1, 4 do
    local t = {[2] = 2, [3] = 3}
    local len1 = #t
    t.x = 1
    res = #t
  end
  local t = {[2] = 2, [3] = 3}; t.x = 1
  return res == #t, "jit=" .. res .. " interp=" .. #t
end)

-- 849bbbe7 (#1532): math.floor()/ceil() without an argument must raise "bad argument".
check("849bbbe7 math.floor() without argument errors", function()
  local ok1, e1 = pcall(math.floor)
  local ok2, e2 = pcall(math.ceil)
  return (not ok1) and (not ok2), "floor: " .. tostring(ok1) .. " " .. tostring(e1) .. " | ceil: " .. tostring(ok2) .. " " .. tostring(e2)
end)

-- bc9261a3 (#1432 #1433 #1438 #1526): for loops with a +-0 step must not be recorded; JIT must match interpreter.
-- Differential: same loop matrix with the JIT off and on (hotloop=1). +0 counts as ascending, -0 as descending.
local function forloops()
  local out = {}
  for _ = 1, 3 do
    for _, a in ipairs({1, 3}) do for _, b in ipairs({1, 3}) do
      for _, s in ipairs({1, -1, 0, -0.0, 1, -0.0, -1, 0}) do
        local n = 0
        for i = a, b, s do n = n + 1; if n >= 50 then break end end
        out[#out + 1] = n
      end
    end end
  end
  return table.concat(out, ",")
end
check("bc9261a3 for loops with variable +-0 step (JIT vs interp)", function()
  jit.off(); jit.flush(); local i = forloops(); jit.on(); jit.flush()
  local j = forloops()
  local diff = 0
  for x, y in string.gmatch(i .. "|" .. j, "") do end
  local ti, tj = {}, {}
  for v in i:gmatch("%d+") do ti[#ti + 1] = v end
  for v in j:gmatch("%d+") do tj[#tj + 1] = v end
  for k = 1, #ti do if ti[k] ~= tj[k] then diff = diff + 1 end end
  return diff == 0, diff .. " of " .. #ti .. " loops differ"
end)

-- 2a8e017e (#911 #1523): tonumber(s, base) must use 64-bit conversion on Windows (unsigned long is 32 bit).
check("2a8e017e tonumber('ffffffffff', 16)", function()
  local v = tonumber("ffffffffff", 16)
  return v == 1099511627775, "got " .. string.format("%.17g", v or -1)
end)

-- ea9eb7ed (#1539): no constant folding of ldexp with a non-integer exponent (JIT must match interpreter).
check("ea9eb7ed math.ldexp(k, non-integer k) JIT vs interp", function()
  local function f()
    local r = {}
    for _ = 1, 60 do
      r[1] = math.ldexp(1, 0.5); r[2] = math.ldexp(3, -0.5); r[3] = math.ldexp(1, 2.5)
      r[4] = math.ldexp(1, 1e10); r[5] = math.ldexp(1, -1e10); r[6] = math.ldexp(1, 0/0)
      r[7] = math.ldexp(1, 2^31 + 0.5); r[8] = math.ldexp(5, -2^40)
    end
    for k = 1, #r do r[k] = string.format("%.17g", r[k]) end
    return table.concat(r, " ")
  end
  jit.off(); jit.flush(); local i = f(); jit.on(); jit.flush()
  local j = f()
  return i == j, "interp=[" .. i .. "] jit=[" .. j .. "]"
end)

-- a4c06453 (#1535): select() index far beyond the varargs must give nil, in and out of traces.
check("a4c06453 select(2^28+k, ...) beyond varargs", function()
  local function f(n, ...) return (select(n, ...)) end
  local bad = 0
  for i = 1, 200 do
    if f(0x10000000 + (i % 3), 1, 2, 3) ~= nil then bad = bad + 1 end
    if f(2, 1, 2, 3) ~= 2 then bad = bad + 1 end
  end
  return bad == 0, "mismatches=" .. bad
end)
