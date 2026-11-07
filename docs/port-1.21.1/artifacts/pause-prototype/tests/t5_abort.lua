-- Test 5: hard abort while a deep, constantly paused coroutine chain is
-- active must terminate within milliseconds (TIMEOUT), including loops that
-- swallow errors with pcall at every level, loops inside non-yieldable C
-- callbacks under xpcall with a looping handler, and a hard abort that
-- arrives while the machine is paused. Soft abort ("Too long without
-- yielding") must stay catchable inside nested coroutines, keep its
-- "chunk:line:" prefix, and propagate as an error when not caught.
local cc = host.include("cc")
local on_soft = nil

-- Build a 6-level chain (alternating create/resume and wrap). Every manager
-- level swallows errors with pcall in an endless loop, so only an
-- uncatchable abort can stop it.
local function chain(depth, inner)
  local function level(k)
    return function()
      host.tag(k)
      if k == depth then return inner() end
      local mk = (k % 2 == 0) and coroutine.wrap or function(f)
        local co = coroutine.create(f)
        return function(...)
          local r = table.pack(coroutine.resume(co, ...))
          if not r[1] then error(r[2], 0) end
          return table.unpack(r, 2, r.n)
        end
      end
      while true do
        local ok, err = pcall(function()
          local child = mk(level(k + 1))
          while true do
            cc.work(k, 20000)
            child()
            coroutine.yield()
          end
        end)
        if not ok and on_soft and tostring(err):find("Too long without yielding") then on_soft() end
      end
    end
  end
  return level(1)()
end

local function spin() local x = 1 while true do x = (x * 48271 + 1) % 2147483647 end end

local cases = {}
local function hard_case(name, delay_us, inner, extra)
  local c = {
    compare_ref = false, expect = "timeout", max_abort_ms = 10,
    main = function()
      host.request("hard", delay_us)
      return chain(6, inner)
    end,
  }
  for k, v in pairs(extra or {}) do c[k] = v end
  cases[name] = c
end

hard_case("hard_pcall_loops", 150000, function()
  while true do pcall(function() while true do cc.work(1, 100000) end end) end
end)
hard_case("hard_xpcall_sort_loop", 170000, function()
  local t = { 3, 1, 2, 5, 4 }
  while true do
    xpcall(function() table.sort(t, function(a, b) spin() end) end, function(e) spin() end)
  end
end)
hard_case("hard_gsub_loop", 190000, function()
  while true do pcall(string.gsub, "abc", "%w", function() spin() end) end
end)
hard_case("hard_in_manager", 230000, function()
  -- The innermost yields quickly; the managers above do the work. The abort
  -- lands in manager code while children are suspended.
  while true do cc.work(1, 1000) coroutine.yield() end
end)
hard_case("hard_while_paused", 0, function()
  while true do cc.work(1, 100000) end
end, { hard_while_paused = 200, main = function() return chain(6, function()
  while true do pcall(function() while true do cc.work(1, 100000) end end) end
end) end })
hard_case("hard_while_deferring", 210000, function()
  -- A pause is pending but deferred (comparator), then the hard abort comes.
  local t = {}
  for i = 1, 500 do t[i] = (i * 37) % 501 end
  while true do
    pcall(table.sort, t, function(a, b) cc.work(a, 2000) return a < b end)
  end
end)

-- Soft abort caught inside the innermost coroutine; the program then
-- finishes normally (with pauses continuing to work).
cases.soft_caught = {
  main = function()
    host.request("soft", 120000)
    local result
    local co = coroutine.create(function()
      local w = coroutine.wrap(function()
        local ok, err = pcall(function() local x = 1 while true do x = (x * 48271 + 1) % 2147483647 end end)
        coroutine.yield(ok, err)
        return cc.work(7, 3000000)
      end)
      local ok, err = w()
      if ok then error("soft abort not raised", 0) end
      if not tostring(err):match("^.+:%d+: Too long without yielding$") then error("bad soft abort message: " .. tostring(err), 0) end
      return err, w()
    end)
    local ok, err, v = coroutine.resume(co)
    if not ok then error(err, 0) end
    return (err:gsub("^.-:", "<chunk>:")), v
  end,
  check = function(kind, text, st, paused)
    if st.nsoft ~= 1 then return false, "soft aborts raised: " .. st.nsoft end
    if paused and st.npause < 20 then return false, "too few pauses " .. st.npause end
    return true, ("soft=%d pauses=%d"):format(st.nsoft, st.npause)
  end,
}

-- Soft abort not caught: propagates through every level as an error.
cases.soft_uncaught = {
  expect = "error", error_contains = "Too long without yielding",
  main = function()
    host.request("soft", 120000)
    local function lvl(k)
      if k == 5 then local x = 1 while true do x = (x * 48271 + 1) % 2147483647 end end
      local co = coroutine.create(function() return lvl(k + 1) end)
      local ok, err = coroutine.resume(co)
      if not ok then error(err, 0) end
    end
    lvl(1)
  end,
}

-- Soft abort caught, the program keeps spinning: the hard abort follows.
cases.soft_then_hard = {
  compare_ref = false, expect = "timeout", max_abort_ms = 10,
  main = function()
    host.request("soft", 80000)
    on_soft = function() host.request("hard", 100000) end
    return chain(6, function()
      while true do
        local ok, err = pcall(function() while true do cc.work(1, 100000) end end)
        if not ok and tostring(err):find("Too long without yielding") then on_soft() end
      end
    end)
  end,
}

return {
  order = { "hard_pcall_loops", "hard_xpcall_sort_loop", "hard_gsub_loop", "hard_in_manager",
            "hard_while_paused", "hard_while_deferring", "soft_caught", "soft_uncaught", "soft_then_hard" },
  cases = cases,
}
