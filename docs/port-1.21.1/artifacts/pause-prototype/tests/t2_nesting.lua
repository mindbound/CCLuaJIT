-- Test 2: deep nesting (7 levels, like bios -> multishell -> shell -> program
-- -> library generators), mixing coroutine.create/resume managers and
-- coroutine.wrap generators. Every level does its own CPU work, so pauses
-- land at every level. Events are routed down like multishell does it, and
-- the innermost filter ("tick") is passed up to the host, which drops
-- non-matching events (CC:T filter semantics).
-- Pass: same result as the run without pauses, and pauses at every level.
local cc = host.include("cc")
local scale = tonumber(os.getenv("SCALE") or "1")

local DEPTH = 7
local R = 600                                   -- ticks consumed by the innermost
local INNER = math.floor(40000 * scale)
local MGR = math.floor(4000 * scale)

local function make_case(order_wrap)
  local results = {}
  local function level(k)
    return function(...)
      host.tag(k)
      local me = coroutine.running()
      local h = k * 1000
      if k == DEPTH then
        local first = ...
        if first ~= "start" then error("innermost: bad start args", 0) end
        for r = 1, R do
          h = cc.work(h, INNER)
          local ev, p = cc.pullEvent("tick")
          if ev ~= "tick" then error("innermost: spurious resume at round " .. r .. ": " .. tostring(ev), 0) end
          if coroutine.status(me) ~= "running" or coroutine.running() ~= me then
            error("innermost: status/running wrong", 0)
          end
          h = cc.mix(h, p)
        end
        results[k] = h
        return "done", h
      end
      local use_wrap = order_wrap(k)
      local child
      if use_wrap then child = coroutine.wrap(level(k + 1)) else child = coroutine.create(level(k + 1)) end
      local args = table.pack(...)
      local steps = 0
      while true do
        h = cc.work(h, MGR * k)
        local r
        if use_wrap then
          r = table.pack(child(table.unpack(args, 1, args.n)))
        else
          r = table.pack(coroutine.resume(child, table.unpack(args, 1, args.n)))
          if not r[1] then error(r[2], 0) end
          if r.n < 1 then error("level " .. k .. ": empty resume result", 0) end
          table.remove(r, 1); r.n = r.n - 1
          if coroutine.status(child) == "running" or coroutine.status(child) == "normal" then
            error("level " .. k .. ": child status " .. coroutine.status(child), 0)
          end
        end
        steps = steps + 1
        h = cc.work(cc.mix(h, steps), MGR * k)
        if r[1] == "done" then
          results[k] = h .. "/" .. steps
          return "done", cc.mix(h, r[2])
        end
        if r[1] ~= "tick" then error("level " .. k .. ": unexpected yield " .. tostring(r[1]), 0) end
        -- Pass the filter up and route the next event down (multishell style).
        args = table.pack(coroutine.yield(table.unpack(r, 1, r.n)))
      end
    end
  end

  return {
    main = function()
      local r = table.pack(level(1)("start"))
      return cc.serialize(results) .. " " .. tostring(r[1]) .. " " .. tostring(r[2])
    end,
    events = function(i)
      if i > 4 * R + 4 then return nil end
      if i % 4 == 0 then return "tick", i end
      if i % 4 == 1 then return "key", i end
      if i % 4 == 2 then return "terminate_not", i end
      return "timer", i
    end,
    check = function(kind, text, st, paused)
      local parts = {}
      for k = 1, DEPTH do parts[#parts + 1] = k .. ":" .. (st.tagpause[k] or 0) end
      local msg = "pauses per level " .. table.concat(parts, " ")
      if paused then
        for k = 1, DEPTH do
          if (st.tagpause[k] or 0) < 1 then return false, "no pause at level " .. k .. "; " .. msg end
        end
      end
      return st.nfailprop == 0, msg
    end,
  }
end

return {
  order = { "alternating", "all_resume", "all_wrap" },
  cases = {
    alternating = make_case(function(k) return k % 2 == 1 end),
    all_resume = make_case(function(k) return false end),
    all_wrap = make_case(function(k) return true end),
  },
}
