-- Test 1: ROM parallel.waitForAny/waitForAll (logic copied from the 1.121.0 ROM)
-- running 3 CPU-heavy routines + 1 event-waiting routine (+ a rednet-like
-- routine), like bios.lua: pcall(parallel.waitForAny, shell, rednet.run).
-- Pass criteria (checked by the host against a run without pauses):
-- identical result string (all checksums, every event the waiter saw, number
-- of scheduler resumes), identical host-level yields/events, no STALL.
local cc = host.include("cc")
local scale = tonumber(os.getenv("SCALE") or "1")

local NT = 1500                          -- ticks every CPU routine consumes
local CH = math.floor(60000 * scale)     -- CPU work per tick (loop iterations)

local function make_case(variant)
  local results = {}

  local function cpu(name, seed, chunk)
    return function()
      local h, me = seed, coroutine.running()
      for tick = 1, NT do
        h = cc.work(h, chunk)
        -- A generator coroutine inside the routine: one more nesting level.
        local gen = coroutine.wrap(function(a)
          for i = 1, 10 do a = coroutine.yield(cc.work(a + i, 500)) end
          return "end"
        end)
        local a = h
        for _ = 1, 10 do a = gen(a) end
        if gen(a) ~= "end" then error(name .. ": generator protocol broken", 0) end
        h = cc.mix(h, a)
        if coroutine.running() ~= me then error(name .. ": coroutine.running changed", 0) end
        local ev, p = cc.pullEvent("tick")
        if ev ~= "tick" or p == nil then
          error(name .. ": spurious resume at tick " .. tick .. ", got " .. tostring(ev), 0)
        end
        h = cc.mix(h, p)
      end
      results[name] = h
    end
  end

  local function waiter()
    local n, h, names = 0, 0, {}
    while true do
      local ev = table.pack(cc.pullEventRaw())
      n = n + 1
      if ev.n == 0 then error("waiter: empty resume (spurious) after " .. n .. " events", 0) end
      for i = 1, ev.n do h = cc.mix(h, ev[i]) end
      names[ev[1]] = (names[ev[1]] or 0) + 1
      h = cc.work(h, 2000)
      if ev[1] == "done" then break end
    end
    results.waiter = n .. ":" .. h
    results.waiter_names = names
  end

  local modem, nmodem = 0, 0
  local function rednet()
    while true do
      local ev, side, ch, reply, msg = cc.pullEvent("modem_message")
      if ev ~= "modem_message" then error("rednet: spurious resume", 0) end
      nmodem = nmodem + 1
      modem = cc.work(cc.mix(modem, msg), 8000)
    end
  end

  local function shell()
    cc.waitForAll(cpu("A", 1, CH), cpu("B", 2, CH * 2), cpu("C", 3, math.floor(CH / 2)), waiter)
  end

  return {
    main = function()
      local ok, err = pcall(cc.waitForAny, shell, rednet)
      if not ok then error(err, 0) end
      results.modem = nmodem .. ":" .. modem
      results.resumes = cc.resumes
      results.winner = err
      return cc.serialize(results)
    end,
    -- Event script: every 3rd event is a tick; others are key/char/modem/mouse.
    events = function(i)
      if i <= 3 * NT then
        local k = i % 3
        if k == 0 then return "tick", i end
        if k == 1 then
          local j = math.floor(i / 3) % 4
          if j == 0 then return "key", 30 + i % 50, false end
          if j == 1 then return "char", string.char(97 + i % 26) end
          if j == 2 then return "modem_message", "left", 42, 7, "msg" .. i, 12.5 end
          return "timer", i
        end
        return "mouse_click", 1, i % 51, i % 19
      elseif i == 3 * NT + 1 then
        return "done"
      end
      return nil
    end,
    check = function(kind, text, st, paused)
      if paused and st.npause < 1000 then return false, "too few pauses: " .. st.npause end
      return st.nfailprop == 0, ("pauses=%d defers=%d failprop=%d"):format(st.npause, st.ndefer, st.nfailprop)
    end,
  }
end

return {
  order = { "parallel" },
  cases = { parallel = make_case("paused") },
}
