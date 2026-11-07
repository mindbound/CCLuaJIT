-- Test 3: user-visible coroutine semantics while pauses happen all the time.
-- Every case does heavy work between the checked operations, so pauses land
-- inside the coroutines being checked. Each case asserts semantics itself
-- (an assertion failure is an ERROR result) and must also match the run
-- without pauses.
local cc = host.include("cc")
local scale = tonumber(os.getenv("SCALE") or "1")
local W = math.floor(150000 * scale)

local function check_pauses(kind, text, st, paused)
  if paused and st.npause < 50 then return false, "too few pauses: " .. st.npause end
  return st.nfailprop == 0, ("pauses=%d defers=%d"):format(st.npause, st.ndefer)
end

local function fail(...) error(table.concat({ ... }, " "), 0) end
local function eq(a, b, what) if a ~= b then fail(what, "expected", tostring(b), "got", tostring(a)) end end

local cases = {}

-- 1. Yields with zero values, nil values and many values; resume arguments.
cases.yield_values = {
  main = function()
    local h = 0
    local shapes = {
      table.pack(), table.pack(nil), table.pack(nil, nil), table.pack(1, nil, 3), table.pack("a", false),
      table.pack(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20),
    }
    local co = coroutine.create(function(...)
      for r = 1, 300 do
        h = cc.work(h + r, W)
        local shape = shapes[r % #shapes + 1]
        local back = table.pack(coroutine.yield(table.unpack(shape, 1, shape.n)))
        eq(back.n, r % 4, "resume arg count at round " .. r)
        for i = 1, back.n do eq(back[i], r * 10 + i, "resume arg") end
      end
      return "fin", h
    end)
    local total = 0
    for r = 1, 300 do
      local shape = shapes[r % #shapes + 1]
      local n = shape.n
      local res
      if r == 1 then
        res = table.pack(coroutine.resume(co, "start"))
      else
        local a = {}
        for i = 1, (r - 1) % 4 do a[i] = (r - 1) * 10 + i end
        res = table.pack(coroutine.resume(co, table.unpack(a, 1, (r - 1) % 4)))
      end
      eq(res[1], true, "resume ok")
      eq(res.n, n + 1, "yielded value count at round " .. r)
      for i = 1, n do eq(res[i + 1], shape[i], "yielded value " .. i .. " at round " .. r) end
      eq(coroutine.status(co), "suspended", "status after yield")
      total = total + res.n
    end
    local a = {}
    for i = 1, 300 % 4 do a[i] = 300 * 10 + i end
    local res = table.pack(coroutine.resume(co, table.unpack(a, 1, 300 % 4)))
    eq(res[1], true, "final") eq(res[2], "fin", "final value")
    eq(coroutine.status(co), "dead", "status after return")
    -- wrap generators: zero-value yields must stay zero values.
    local gen = coroutine.wrap(function()
      for r = 1, 200 do
        h = cc.work(h, W)
        if r % 2 == 0 then coroutine.yield() else coroutine.yield(r, nil) end
      end
    end)
    for r = 1, 200 do
      local n = select("#", gen())
      eq(n, (r % 2 == 0) and 0 or 2, "wrap yield count at round " .. r)
    end
    return total, h
  end,
  check = check_pauses,
}

-- 2. coroutine.status / running / isyieldable, during and after pauses.
cases.status_running = {
  main = function()
    local log = {}
    local outer_co = coroutine.running()
    local function chk(cond, what) if not cond then fail("status check failed:", what) end end
    local mid, inner
    mid = coroutine.create(function()
      local me = coroutine.running()
      chk(me == mid, "running() is mid")
      inner = coroutine.create(function()
        local me2, ismain = coroutine.running()
        chk(me2 == inner and ismain == false, "running() is inner, not main")
        local h = 0
        for r = 1, 150 do
          h = cc.work(h, W)
          chk(coroutine.running() == me2, "running stable across pauses")
          chk(coroutine.status(me2) == "running", "self running")
          chk(coroutine.status(mid) == "normal", "parent normal: " .. coroutine.status(mid))
          chk(coroutine.status(outer_co) == "normal", "grandparent normal")
          chk(coroutine.isyieldable() == true, "isyieldable in coroutine")
          -- Inside a C callback LuaJIT reports not yieldable (Cobalt: true).
          local cb
          string.gsub("x", "x", function() cb = coroutine.isyieldable() end)
          chk(cb == false, "not yieldable inside gsub callback (LuaJIT semantics)")
          coroutine.yield(r)
        end
        return "inner done", h
      end)
      local h = 0
      while true do
        h = cc.work(h, W)
        local ok, v, w = coroutine.resume(inner)
        chk(ok, "inner resume ok")
        chk(coroutine.running() == me, "mid running stable")
        if coroutine.status(inner) == "dead" then
          chk(v == "inner done", "inner result")
          log[#log + 1] = w
          break
        end
        chk(coroutine.status(inner) == "suspended", "inner suspended after yield")
        log[#log + 1] = v
        coroutine.yield(v)
      end
      return "mid done", h
    end)
    local n = 0
    while coroutine.status(mid) ~= "dead" do
      local ok, v = coroutine.resume(mid)
      chk(ok, "mid resume: " .. tostring(v))
      n = n + 1
    end
    local ok, err = coroutine.resume(mid)
    chk(not ok and err == "cannot resume dead coroutine", "dead resume msg: " .. tostring(err))
    return n, table.concat(log, ",")
  end,
  check = check_pauses,
}

-- 3. Errors raised inside paused coroutines; wrap re-raise semantics.
cases.errors = {
  main = function()
    local out = {}
    local function heavy_then_error(v)
      local h = cc.work(1, W * 20)  -- paused many times
      error(v, v == "boom" and 1 or 0)
    end
    -- (a) string error with position
    local co = coroutine.create(function() heavy_then_error("boom") end)
    local ok, err = coroutine.resume(co)
    if ok or not tostring(err):match("t3_semantics%.lua:%d+: boom$") then fail("string error:", tostring(err)) end
    out[#out + 1] = err
    eq(coroutine.status(co), "dead", "status after error")
    local tb = debug.traceback(co)
    if not tb:find("heavy_then_error") then fail("traceback of dead coroutine lacks frame:", tb) end
    -- (b) table error identity
    local E = { tag = "E" }
    co = coroutine.create(function() heavy_then_error(E) end)
    ok, err = coroutine.resume(co)
    if ok or not rawequal(err, E) then fail("table error identity lost") end
    -- (c) error through a 3-level chain of managers that rethrow
    local function manager(f)
      return function()
        local c = coroutine.create(f)
        while true do
          cc.work(5, W)
          local ok2, e = coroutine.resume(c)
          if not ok2 then error(e, 0) end
          if coroutine.status(c) == "dead" then return end
          coroutine.yield()
        end
      end
    end
    co = coroutine.create(manager(manager(function()
      for i = 1, 5 do cc.work(i, W) coroutine.yield() end
      heavy_then_error("deep")
    end)))
    repeat ok, err = coroutine.resume(co) until not ok or coroutine.status(co) == "dead"
    eq(ok, false, "deep error propagated") eq(err, "deep", "deep error value")
    -- (d) wrap: errors re-raised unchanged (Cobalt semantics, no extra prefix)
    local gen = coroutine.wrap(function() coroutine.yield(1) heavy_then_error("boom") end)
    eq(gen(), 1, "wrap first value")
    ok, err = pcall(gen)
    if ok or not tostring(err):match("t3_semantics%.lua:%d+: boom$") then fail("wrap string error:", tostring(err)) end
    out[#out + 1] = err
    gen = coroutine.wrap(function() heavy_then_error(E) end)
    ok, err = pcall(gen)
    if ok or not rawequal(err, E) then fail("wrap table error identity lost") end
    -- dead wrap: error with the caller's position
    ok, err = pcall(function() local v = gen() return v end)
    if ok or not tostring(err):match("t3_semantics%.lua:%d+: cannot resume dead coroutine$") then
      fail("dead wrap:", tostring(err))
    end
    out[#out + 1] = (tostring(err):gsub("%d+", "N"))
    -- (e) resuming running / normal / dead coroutines
    local self_co
    self_co = coroutine.create(function()
      local r1 = table.pack(coroutine.resume(self_co))
      local parent = co
      return r1[1], r1[2]
    end)
    local _, a, b = coroutine.resume(self_co)
    eq(a, false, "resume self") eq(b, "cannot resume running coroutine", "resume self msg")
    local outer = coroutine.running()
    local nco = coroutine.create(function() return coroutine.resume(outer) end)
    local _, a2, b2 = coroutine.resume(nco)
    eq(a2, false, "resume normal") eq(b2, "cannot resume normal coroutine", "resume normal msg")
    -- (f) a wrapped generator calling itself
    local selfgen
    selfgen = coroutine.wrap(function() return pcall(selfgen) end)
    local okself, eself = selfgen()
    if okself or not tostring(eself):match("cannot resume running coroutine$") then fail("wrap self:", tostring(eself)) end
    -- (g) bad arguments
    ok, err = pcall(coroutine.resume, 42)
    if ok or not tostring(err):find("coroutine expected") then fail("bad arg:", tostring(err)) end
    ok, err = pcall(coroutine.wrap, 42)
    if ok or not tostring(err):find("function expected") then fail("bad wrap arg:", tostring(err)) end
    return table.concat(out, " | ")
  end,
  check = check_pauses,
}

-- 4. The top-level coroutine yields zero values itself (os.pullEventRaw()
-- with no filter) and filtered values; the host must not confuse these with
-- pauses, and must keep the filter across pauses.
cases.toplevel_yields = {
  main = function()
    local h, got = 0, {}
    for r = 1, 400 do
      h = cc.work(h, W)
      local ev = table.pack(coroutine.yield())              -- zero values: no filter
      got[#got + 1] = ev[1] .. ":" .. tostring(ev[2])
      h = cc.work(cc.mix(h, ev[2]), W)
      local ev2 = table.pack(coroutine.yield("b"))          -- filter "b"
      if ev2[1] ~= "b" then fail("filter violated: got", tostring(ev2[1])) end
      got[#got + 1] = ev2[1] .. ":" .. tostring(ev2[2])
    end
    return h, #got, table.concat(got, ","):sub(1, 400)
  end,
  events = function(i)
    if i > 3000 then return nil end
    local names = { "a", "b", "c" }
    return names[i % 3 + 1], i
  end,
  check = check_pauses,
}

-- 5. Yield across pcall/xpcall bodies inside paused coroutines.
cases.pcall_yield = {
  main = function()
    local co = coroutine.create(function()
      local acc = {}
      for r = 1, 100 do
        local ok, a, b = pcall(function()
          cc.work(r, W)
          local x, y = coroutine.yield("in pcall", r)
          cc.work(x, W)
          if r % 3 == 0 then error("e" .. r, 0) end
          return x, y
        end)
        acc[#acc + 1] = tostring(ok) .. tostring(a) .. tostring(b)
        local ok2, m = xpcall(function()
          local z = coroutine.yield("in xpcall")
          cc.work(z, W)
          error({ z = z })
        end, function(e) return "handled" .. e.z end)
        acc[#acc + 1] = tostring(ok2) .. m
      end
      return table.concat(acc, ";")
    end)
    local r, res = 0, nil
    while coroutine.status(co) ~= "dead" do
      r = r + 1
      local ok, v, w = coroutine.resume(co, r, r * 2)
      if not ok then fail("pcall_yield:", tostring(v)) end
      res = v
    end
    return r, res:sub(1, 300)
  end,
  check = check_pauses,
}

-- 6. Coroutine-heavy code under very frequent pauses: most pauses land in
-- the resume/wrap wrappers themselves or between resume and yield.
cases.pingpong_under_pauses = {
  pause_min_us = 20, pause_max_us = 60,
  main = function()
    local N = math.floor(400000 * scale)
    local leaf = coroutine.create(function(x)
      local i = 0
      while true do i = i + 1 x = coroutine.yield(i, x) end
    end)
    local mid = coroutine.wrap(function(x)
      while true do
        local ok, a, b = coroutine.resume(leaf, x)
        if not ok then error(a, 0) end
        x = coroutine.yield(a + b)
      end
    end)
    local gen = coroutine.wrap(function()
      for i = 1, N do coroutine.yield(mid(i)) end
      return "end"
    end)
    local s, n = 0, 0
    while true do
      local v = gen()
      if v == "end" then break end
      s = s + v n = n + 1
    end
    if n ~= N or s ~= N * (N + 1) then error(("pingpong broken: n=%d s=%.0f"):format(n, s), 0) end
    return n, s
  end,
  check = function(kind, text, st, paused)
    if paused and st.npause < 100 then return false, "too few pauses: " .. st.npause end
    return st.nfailprop == 0, ("pauses=%d"):format(st.npause)
  end,
}

return {
  order = { "yield_values", "status_running", "errors", "toplevel_yields", "pcall_yield", "pingpong_under_pauses" },
  cases = cases,
}
