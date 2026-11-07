-- Test 7: cost of the resume/wrap wrappers vs LuaJIT's native fast functions.
-- Runs with the permanent count hook installed (production config) and no
-- pause requests (case "micro"), then measures what pauses themselves cost
-- on a CPU-bound 5-level chain ("pause_cost", compared with its reference).
local cc = host.include("cc")
local scale = tonumber(os.getenv("SCALE") or "1")

local function best_of(n, f)
  local best = math.huge
  for _ = 1, n do
    local t0 = host.now()
    f()
    local dt = host.now() - t0
    if dt < best then best = dt end
  end
  return best
end

local N = math.floor(2000000 * scale)

local function pingpong(resume, create, yield)
  return function()
    local co = create(function() local i = 0 while true do i = i + 1 yield(i) end end)
    local s = 0
    for _ = 1, N do local ok, v = resume(co) s = s + v end
    assert(s == N * (N + 1) / 2)
  end
end

local function generator(wrap, yield)
  return function()
    local g = wrap(function() local i = 0 while true do i = i + 1 yield(i) end end)
    local s = 0
    for _ = 1, N do s = s + g() end
    assert(s == N * (N + 1) / 2)
  end
end

local function nested3(resume, create, yield)
  -- Three levels: every value is passed through two manager coroutines.
  return function()
    local function pass(inner)
      return create(function()
        while true do local ok, v = resume(inner) yield(v) end
      end)
    end
    local leaf = create(function() local i = 0 while true do i = i + 1 yield(i) end end)
    local top = pass(pass(leaf))
    local s = 0
    for _ = 1, N / 4 do local ok, v = resume(top) s = s + v end
  end
end

local function scheduler(resume, create, yield, status)
  -- parallel.waitForAll-shaped round robin over 4 coroutines, events fed in.
  return function()
    local cos, filters = {}, {}
    for i = 1, 4 do
      cos[i] = create(function()
        local n = 0
        while true do local ev, p = yield(i % 2 == 0 and "tick" or nil) n = n + (p or 0) end
      end)
    end
    for i = 1, 4 do local ok, f = resume(cos[i]) filters[i] = f end
    for e = 1, N / 4 do
      local name = (e % 2 == 0) and "tick" or "key"
      for i = 1, 4 do
        if filters[i] == nil or filters[i] == name then
          local ok, f = resume(cos[i], name, e)
          if status(cos[i]) == "dead" then error("dead") end
          filters[i] = f
        end
      end
    end
  end
end

local cases = {}
cases.micro = {
  pauses = false, compare_ref = false,
  main = function()
    local C = coroutine
    local nres, nwrap = host.native_resume, host.native_wrap
    local rows = {}
    local function row(name, fn_native, fn_wrapped, ops)
      local tn = best_of(5, fn_native)
      local tw = best_of(5, fn_wrapped)
      rows[#rows + 1] = ("%-22s native %7.1f ns/op  wrapper %7.1f ns/op  ratio %.2fx")
        :format(name, tn * 1000 / ops, tw * 1000 / ops, tw / tn)
    end
    row("resume/yield pingpong", pingpong(nres, C.create, C.yield), pingpong(C.resume, C.create, C.yield), N)
    -- Split: the C part alone (raw lua_resume wrapper without the Lua continuation).
    local raw
    for i = 1, 10 do
      local name, v = debug.getupvalue(C.resume, i)
      if name == "raw_resume" then raw = v end
    end
    if raw then row("  (C part only)", pingpong(nres, C.create, C.yield), pingpong(raw, C.create, C.yield), N) end
    row("wrap generator", generator(nwrap, C.yield), generator(C.wrap, C.yield), N)
    row("3-level pass-through", nested3(nres, C.create, C.yield), nested3(C.resume, C.create, C.yield), N / 4)
    row("4-coroutine scheduler", scheduler(nres, C.create, C.yield, C.status),
        scheduler(C.resume, C.create, C.yield, C.status), N / 4)
    for _, r in ipairs(rows) do host.print("  BENCH " .. r) end
    return #rows
  end,
}

-- What the pauses themselves cost: CPU-bound work 5 levels deep, run with
-- and without pause requests every 0.3-1.5 ms (host compares results).
cases.pause_cost = {
  main = function()
    local function level(k)
      if k == 5 then
        local h = 0
        for r = 1, 40 do h = cc.work(h + r, math.floor(2000000 * scale)) end
        return h
      end
      local co = coroutine.create(function() return level(k + 1) end)
      local ok, v = coroutine.resume(co)
      if not ok then error(v, 0) end
      return v
    end
    return level(1)
  end,
  check = function(kind, text, st, paused)
    return true, ("pauses=%d"):format(st.npause)
  end,
}

-- Same but with interpreter-heavy work (string/table ops), where the count
-- hook runs every 10^4 instructions.
cases.pause_cost_interp = {
  main = function()
    local function level(k)
      if k == 5 then
        local h = 0
        for r = 1, math.floor(40000 * scale) do
          local t = {}
          for i = 1, 20 do t[i] = tostring(i * r) end
          h = cc.mix(h, table.concat(t, ":"))
        end
        return h
      end
      local co = coroutine.create(function() return level(k + 1) end)
      local ok, v = coroutine.resume(co)
      if not ok then error(v, 0) end
      return v
    end
    return level(1)
  end,
  check = function(kind, text, st, paused)
    return true, ("pauses=%d"):format(st.npause)
  end,
}

return { order = { "micro", "pause_cost", "pause_cost_interp" }, cases = cases }
