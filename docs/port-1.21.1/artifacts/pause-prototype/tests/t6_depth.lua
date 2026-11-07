-- Test 6: coroutine nesting limit on a VM thread with a 1 MB stack.
-- Deep coroutine recursion must fail with a clean Lua error at the limit
-- (resume -> false, wrap -> error), never overflow the native stack.
-- Also measures native stack bytes per nesting level.
local cc = host.include("cc")
local LIMIT = host.max_depth

local cases = {}

local function strip(s) return (tostring(s):gsub("^.-:%d+: ", "")) end

cases.wrap_recursion = {
  main = function()
    local maxd, s1, s51 = 0, 0, 0
    local function nest(n)
      maxd = n
      if n == 1 then s1 = host.stack_used() elseif n == 51 then s51 = host.stack_used() end
      local g = coroutine.wrap(function() return nest(n + 1) end)
      return g()
    end
    local ok, err = pcall(nest, 1)
    return tostring(ok), strip(err), maxd, "bytes/level=" .. (s51 - s1) / 50
  end,
  check = function(kind, text, st)
    if not text:find("coroutine nesting too deep") then return false, "no nesting error" end
    return st.depth_hw == LIMIT, ("depth_hw=%d stack_hw=%d"):format(st.depth_hw, st.stack_hw)
  end,
}

cases.resume_recursion = {
  main = function()
    local maxd, s1, s51 = 0, 0, 0
    local function nest(n)
      maxd = n
      if n == 1 then s1 = host.stack_used() elseif n == 51 then s51 = host.stack_used() end
      local co = coroutine.create(function() return nest(n + 1) end)
      local ok, a, b = coroutine.resume(co)
      if not ok then return "err@" .. n, a end
      return a, b
    end
    local where, msg = nest(1)
    return where, strip(msg), maxd, "bytes/level=" .. (s51 - s1) / 50
  end,
  check = function(kind, text, st)
    if not text:find("coroutine nesting too deep") then return false, "no nesting error" end
    return st.depth_hw == LIMIT, ("depth_hw=%d stack_hw=%d"):format(st.depth_hw, st.stack_hw)
  end,
}

-- Every level also passes through a C callback (string.gsub), the most
-- stack-hungry way to nest coroutines.
local function gsub_nest(limit_note)
  return function()
    local maxd, s1, s51 = 0, 0, 0
    local function nest(n)
      maxd = n
      if n == 1 then s1 = host.stack_used() elseif n == 51 then s51 = host.stack_used() end
      local res
      string.gsub("x", "x", function()
        local co = coroutine.create(function() return nest(n + 1) end)
        local ok, a = coroutine.resume(co)
        res = ok and a or ("err@" .. n .. ": " .. strip(a))
      end)
      return res
    end
    return nest(1), maxd, "bytes/level=" .. (s51 - s1) / 50, limit_note
  end
end
cases.gsub_mixed_recursion = {
  main = gsub_nest("count limit"),
  check = function(kind, text, st)
    -- Windows (512-byte luaL_Buffer): the count limit fires first. Linux
    -- (8 KB luaL_Buffer per gsub frame): the stack floor fires first.
    if not (text:find("coroutine nesting too deep") or text:find("C stack overflow")) then
      return false, "no nesting error"
    end
    return true, ("depth_hw=%d stack_hw=%d"):format(st.depth_hw, st.stack_hw)
  end,
}
-- Same with the count limit disabled: the stack-floor check (256 KB left)
-- must stop it.
cases.gsub_mixed_floor_only = {
  max_depth = 1000000000,
  main = gsub_nest("floor only"),
  check = function(kind, text, st)
    if not text:find("C stack overflow") then return false, "no floor error" end
    return true, ("depth_hw=%d stack_hw=%d floor_errors=%d"):format(st.depth_hw, st.stack_hw, st.nfloor_err)
  end,
}
cases.wrap_floor_only = {
  max_depth = 1000000000,
  main = function()
    local maxd = 0
    local function nest(n)
      maxd = n
      local g = coroutine.wrap(function() return nest(n + 1) end)
      return g()
    end
    local ok, err = pcall(nest, 1)
    return tostring(ok), strip(err), maxd
  end,
  check = function(kind, text, st)
    if not text:find("C stack overflow") then return false, "no floor error" end
    return true, ("depth_hw=%d stack_hw=%d floor_errors=%d"):format(st.depth_hw, st.stack_hw, st.nfloor_err)
  end,
}

-- Pauses while LIMIT-1 levels deep: each pause propagates through every level.
cases.pause_at_depth = {
  main = function()
    local function nest(n)
      if n == LIMIT then
        host.tag(1)
        local h = 0
        for r = 1, 60 do h = cc.work(h + r, 500000) end
        return h
      end
      local co = coroutine.create(function() return nest(n + 1) end)
      local ok, v = coroutine.resume(co)
      if not ok then error(v, 0) end
      return v
    end
    return nest(1)
  end,
  check = function(kind, text, st, paused)
    if paused and (st.tagpause[1] or 0) < 50 then return false, "too few pauses at the bottom: " .. tostring(st.tagpause[1]) end
    return st.nfailprop == 0, ("pauses=%d propagation steps=%d (%.0f per pause) depth_hw=%d"):format(
      st.npause, st.nprop, st.npause > 0 and st.nprop / st.npause or 0, st.depth_hw)
  end,
}

-- Error raised at the deepest allowed level unwinds cleanly through all.
cases.error_at_depth = {
  main = function()
    local function nest(n)
      if n == LIMIT then cc.work(1, 2000000) error("bottom", 0) end
      local co = coroutine.create(function() return nest(n + 1) end)
      local ok, v = coroutine.resume(co)
      if not ok then error(v, 0) end
      return v
    end
    local ok, e = pcall(nest, 1)
    return tostring(ok), e
  end,
}

return {
  order = { "wrap_recursion", "resume_recursion", "gsub_mixed_recursion", "gsub_mixed_floor_only",
            "wrap_floor_only", "pause_at_depth", "error_at_depth" },
  cases = cases,
}
