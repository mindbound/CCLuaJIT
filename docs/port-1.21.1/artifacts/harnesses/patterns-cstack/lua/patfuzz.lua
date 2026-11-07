-- Differential fuzz for Lua pattern matching (string.find/match/gmatch/gsub).
-- usage: patfuzz.lua <seed> <npatterns> [nonul]
-- Prints one line per (pattern, subject, function) with the serialized result
-- or error message. Two builds must produce byte-identical output.
local seed, npat, nonul = ...
seed = tonumber(seed) or 1
npat = tonumber(npat) or 20000
nonul = nonul == "nonul"

-- Own PRNG (xorshift-ish LCG on doubles kept below 2^31), identical everywhere.
local state = seed % 2147483647
if state <= 0 then state = state + 2147483646 end
local function rnd(n)  -- 1..n
  state = (state * 48271) % 2147483647
  return state % n + 1
end
local function pick(t) return t[rnd(#t)] end

local subjchars = {"a", "a", "a", "b", "b", "c", "(", ")", "x", "1", "2", " ", "%", ".", "-", "[", "]", "A", "\n", "\255", "\0"}
if nonul then subjchars[#subjchars] = "z" end
local atoms = {"a", "b", "c", ".", "%a", "%d", "%l", "%u", "%w", "%s", "%p", "%x", "%c", "%A", "%S", "%W",
  "[ab]", "[^a]", "[a-c]", "[%d]", "[%]]", "[]", "[^]", "[a-]", "%%", "%.", "%(", "x", "1", "(", ")", "()",
  "%b()", "%bab", "%f[%w]", "%f[%W]", "%f[a]", "%1", "%2", "%0", "%", "[", "^", "$", "\0", "%z", "[%z]", "\255"}
if nonul then
  for i = #atoms, 1, -1 do if atoms[i]:find("\0", 1, true) then table.remove(atoms, i) end end
end
local quants = {"", "", "", "*", "+", "-", "?"}

local function genpat()
  local n = rnd(7) - 1
  local t = {}
  if rnd(6) == 1 then t[#t + 1] = "^" end
  local open = 0
  for i = 1, n do
    local r = rnd(10)
    if r == 1 and open < 4 then t[#t + 1] = "("; open = open + 1
    elseif r == 2 and open > 0 then t[#t + 1] = ")"; open = open - 1
    else
      local a = pick(atoms)
      t[#t + 1] = a
      if #a == 1 or a:sub(1, 1) == "[" or a:match("^%%[%a]$") then t[#t + 1] = pick(quants) end
    end
  end
  while open > 0 and rnd(4) > 1 do t[#t + 1] = ")"; open = open - 1 end
  if rnd(6) == 1 then t[#t + 1] = "$" end
  return table.concat(t)
end

local function gensubj()
  local n = rnd(24) - 1
  local t = {}
  for i = 1, n do t[i] = pick(subjchars) end
  return table.concat(t)
end

local function ser(...)
  local t = {}
  for i = 1, select("#", ...) do
    local v = select(i, ...)
    t[i] = type(v) == "string" and string.format("%q", v) or tostring(v)
  end
  return table.concat(t, ",")
end

local function run(f, ...)
  local r = {pcall(f, ...)}
  if r[1] then return "ok:" .. ser(unpack(r, 2, table.maxn(r))) end
  return "err:" .. tostring(r[2])
end

local out = {}
local function emit(s)
  out[#out + 1] = s
  if #out >= 512 then io.write(table.concat(out, "\n"), "\n"); out = {} end
end

local repls = {"<%0>", "%1", "x%%y", "%2%1", "", "%", "%a"}
local rtab = {a = "A", b = false, ["("] = 1}
for ip = 1, npat do
  local p = genpat()
  for is = 1, 5 do
    local s = gensubj()
    local init = rnd(5) == 1 and (rnd(#s + 3) - 2) or nil
    emit(ip .. "." .. is .. " F " .. run(string.find, s, p, init))
    emit(ip .. "." .. is .. " P " .. run(string.find, s, p, init, true))
    emit(ip .. "." .. is .. " M " .. run(string.match, s, p, init))
    emit(ip .. "." .. is .. " G " .. run(function()
      local acc, n = {}, 0
      for a, b, c in string.gmatch(s, p) do
        n = n + 1
        acc[#acc + 1] = ser(a, b, c)
        if n > 200 then break end
      end
      return table.concat(acc, ";")
    end))
    local r = rnd(9)
    if r <= 7 then
      emit(ip .. "." .. is .. " S " .. run(string.gsub, s, p, repls[r], rnd(3) == 1 and rnd(4) - 1 or nil))
    elseif r == 8 then
      emit(ip .. "." .. is .. " T " .. run(string.gsub, s, p, rtab))
    else
      emit(ip .. "." .. is .. " U " .. run(string.gsub, s, p, function(a, b) if a == "a" then return nil end return ser(a, b) end))
    end
  end
end
io.write(table.concat(out, "\n"), "\n")
