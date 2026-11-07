-- Test 4: pauses requested while code runs inside C-call boundaries
-- (table.sort comparator, string.gsub callback, load reader, xpcall handler,
-- metamethods called from C) and inside yieldable constructs (pcall, xpcall
-- body, VM metamethods). Every workload runs 2 coroutine levels below top.
-- host.region(id) marks the code region; the hook counts pauses and
-- deferrals per region.
-- Pass: same result as without pauses (so no "attempt to yield across
-- C-call boundary" ever reached user code), no pause *inside* a
-- non-yieldable region (they are deferred), and pauses do happen inside the
-- yieldable regions.
local cc = host.include("cc")
local scale = tonumber(os.getenv("SCALE") or "1")
local function S(n) return math.floor(n * scale) end

local function rw(id, seed, n)
  host.region(id)
  local v = cc.work(seed, n)
  host.region(0)
  return v
end

-- Run fn two coroutine levels deep (manager with create/resume, worker wrap).
local function nested(fn)
  local mgr = coroutine.create(function()
    local worker = coroutine.wrap(function() return "ok", fn() end)
    return worker()
  end)
  local res = table.pack(coroutine.resume(mgr))
  if not res[1] then error(res[2], 0) end
  return table.unpack(res, 2, res.n)
end

local function obj(v, mt) return setmetatable({ v = v }, mt) end

local workloads = {
  sort_comparator = { id = 1, yieldable = false, fn = function()
    local t = {}
    for i = 1, 2000 do t[i] = (i * 7919) % 2003 end
    table.sort(t, function(a, b) rw(1, a + b, S(3000)) return a < b end)
    return table.concat(t, ",", 1, 20)
  end },
  gsub_callback = { id = 2, yieldable = false, fn = function()
    local s = string.rep("word ", 20000)
    local r, n = s:gsub("%a+", function(w) rw(2, #w, S(5000)) return w:upper() end)
    return r:sub(1, 20), n
  end },
  pcall_body = { id = 3, yieldable = true, fn = function()
    local h = 0
    for i = 1, 100 do local ok, v = pcall(function() return rw(3, i, S(1000000)) end) h = cc.mix(h, v) end
    return h
  end },
  xpcall_body = { id = 4, yieldable = true, fn = function()
    local h = 0
    for i = 1, 100 do
      local ok, v = xpcall(function() return rw(4, i, S(1000000)) end, function(e) return e end)
      h = cc.mix(h, v)
    end
    return h
  end },
  xpcall_handler = { id = 5, yieldable = false, fn = function()
    local h = 0
    for i = 1, 100 do
      local ok, v = xpcall(function() error("x" .. i, 0) end, function(e) return rw(5, #e, S(1000000)) end)
      h = cc.mix(h, v)
    end
    return h
  end },
  index_metamethod = { id = 6, yieldable = true, fn = function()
    local o = obj(1, { __index = function(t, k) return rw(6, k, S(1000000)) end })
    local h = 0
    for i = 1, 100 do h = cc.mix(h, o[i]) end
    return h
  end },
  lt_metamethod = { id = 7, yieldable = true, fn = function()
    local mt = {} mt.__lt = function(a, b) rw(7, a.v, S(1000000)) return a.v < b.v end
    local c = 0
    for i = 1, 100 do if obj(i, mt) < obj(100 - i, mt) then c = c + 1 end end
    return c
  end },
  arith_concat_eq_call_len = { id = 8, yieldable = true, fn = function()
    local mt = {}
    mt.__add = function(a, b) return obj(rw(8, a.v + b.v, S(400000)) % 1000, mt) end
    mt.__concat = function(a, b) return obj(rw(8, a.v * 3 + b.v, S(400000)) % 1000, mt) end
    mt.__eq = function(a, b) rw(8, a.v, S(400000)) return a.v % 2 == b.v % 2 end
    mt.__call = function(self, x) return rw(8, self.v + x, S(400000)) % 1000 end
    mt.__len = function(self) return rw(8, self.v, S(400000)) % 1000 end
    local h = 0
    for i = 1, 30 do
      local a, b = obj(i, mt), obj(i * 2, mt)
      h = cc.mix(h, (a + b).v)
      h = cc.mix(h, (a .. b .. a).v)
      h = cc.mix(h, a == b)
      h = cc.mix(h, a(i))
      h = cc.mix(h, #a)
    end
    return h
  end },
  tostring_metamethod = { id = 9, yieldable = nil, fn = function()
    local mt = { __tostring = function(self) return "o" .. rw(9, self.v, S(1000000)) end }
    local h = 0
    for i = 1, 100 do h = cc.mix(h, tostring(obj(i, mt))) end
    return h
  end },
  format_tostring = { id = 10, yieldable = nil, fn = function()
    local mt = { __tostring = function(self) return "o" .. rw(10, self.v, S(1000000)) end }
    local h = 0
    for i = 1, 100 do h = cc.mix(h, string.format("%s", obj(i, mt))) end
    return h
  end },
  sort_lt_metamethod = { id = 11, yieldable = false, fn = function()
    local mt = { __lt = function(a, b) rw(11, a.v, S(3000)) return a.v < b.v end }
    local t = {}
    for i = 1, 2000 do t[i] = obj((i * 7919) % 2003, mt) end
    table.sort(t)
    local out = {}
    for i = 1, 20 do out[i] = t[i].v end
    return table.concat(out, ",")
  end },
  wrap_inside_gsub = { id = 12, yieldable = false, fn = function()
    -- A generator coroutine resumed from inside a gsub callback: the
    -- generator itself is yieldable, but its resumer is not, so the pause
    -- cannot propagate and must be deferred (nonyield on the chain).
    local s = string.rep("w ", 200)
    local h = 0
    local r = s:gsub("%a", function(w)
      local gen = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(rw(12, i, S(150000))) end end)
      for i = 1, 3 do h = cc.mix(h, gen()) end
      return "v"
    end)
    return #r, h
  end },
  load_reader = { id = 13, yieldable = false, fn = function()
    local parts, i = { "return ", "1", " + ", "2", " + ", "3" }, 0
    local h = 0
    for k = 1, 30 do
      i = 0
      local f = load(function()
        i = i + 1
        if parts[i] then h = cc.mix(h, rw(13, i + k, S(500000))) end
        return parts[i]
      end, "=reader")
      h = cc.mix(h, f())
    end
    return h
  end },
  gsub_sort_nested = { id = 14, yieldable = false, fn = function()
    local h = 0
    local r = string.rep("x", 30):gsub("x", function()
      local t = {}
      for i = 1, 60 do t[i] = (i * 37) % 61 end
      table.sort(t, function(a, b) rw(14, a, S(3000)) return a > b end)
      h = cc.mix(h, t[1] .. t[60])
      return "y"
    end)
    return r:sub(1, 5), h
  end },
  table_foreach = { id = 15, yieldable = nil, fn = function()
    local h = 0
    if not table.foreach then return "no table.foreach" end
    local t = {}
    for i = 1, 100 do t[i] = i end
    table.foreach(t, function(k, v) h = cc.mix(h, rw(15, v, S(1000000))) end)
    return h
  end },
  pairs_metamethod = { id = 16, yieldable = nil, fn = function()
    local mt = { __pairs = function(self)
      local i = 0
      return function() i = i + 1 if i <= 100 then return i, rw(16, i, S(1000000)) end end, self, nil
    end }
    local h = 0
    for k, v in pairs(obj(1, mt)) do h = cc.mix(h, v) end
    return h
  end },
  gmatch_loop = { id = 17, yieldable = true, fn = function()
    local h = 0
    for w in string.rep("ab ", 100):gmatch("%a+") do h = cc.mix(h, rw(17, #w, S(1000000))) end
    return h
  end },
}

local order, cases = {}, {}
local names = {}
for name in pairs(workloads) do names[#names + 1] = name end
table.sort(names, function(a, b) return workloads[a].id < workloads[b].id end)
for _, name in ipairs(names) do
  local w = workloads[name]
  order[#order + 1] = name
  cases[name] = {
    main = function() return nested(w.fn) end,
    check = function(kind, text, st, paused)
      local p, d = st.rpause[w.id], st.rdefer[w.id]
      local msg = ("region %d: pauses inside=%d deferrals inside=%d, total pauses=%d, max deferral=%.0f us -> %s")
        :format(w.id, p, d, st.npause, st.max_defer_us, p > 0 and "YIELDABLE" or (d > 0 and "DEFERRED" or "no hook hit"))
      if text:find("C%-call boundary") then return false, "yield across C-call boundary leaked: " .. msg end
      if not paused then return true, msg end
      if st.nfailprop ~= 0 then return false, "failed propagation; " .. msg end
      if w.yieldable == true and p == 0 then return false, "expected pauses inside; " .. msg end
      if w.yieldable == false and (p ~= 0 or d == 0) then return false, "expected deferral only; " .. msg end
      -- Fairness note: a workload that lives almost entirely inside a
      -- non-yieldable callback may starve pauses (deferred until it ends);
      -- that is the documented limitation, not a failure.
      if st.npause == 0 and w.yieldable ~= false then return false, "no pauses at all; " .. msg end
      return true, msg
    end,
  }
end

return { order = order, cases = cases }
