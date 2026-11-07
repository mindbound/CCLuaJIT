-- ROM-equivalent pieces for the tests: os.pullEvent(Raw) from bios.lua and
-- parallel.waitForAny / waitForAll from rom/apis/parallel.lua (CC:T 1.121.0),
-- with the logic copied verbatim. Only `expect` and the cc.internal.exception
-- helpers are replaced by trivial equivalents (try_barrier keeps its shape).

local M = {}

function M.pullEventRaw(sFilter)
    return coroutine.yield(sFilter)
end

function M.pullEvent(sFilter)
    local eventData = table.pack(M.pullEventRaw(sFilter))
    if eventData[1] == "terminate" then
        error("Terminated", 0)
    end
    return table.unpack(eventData, 1, eventData.n)
end

local os = { pullEventRaw = M.pullEventRaw, pullEvent = M.pullEvent }

-- cc.internal.exception (fallback path of the ROM module)
local function bounce(...) return ... end
local exception = {
    try_barrier = function(parent, f, ...) return bounce(f(...)) end,
    wrap_error = function(err, thread) return err end,
}
local function expect(i, v, t)
    if type(v) ~= t then error(("bad argument #%d (%s expected, got %s)"):format(i, t, type(v)), 3) end
end

-- Instrumentation: number of coroutine.resume calls returning, per scheduler.
M.resumes = 0

function M.waitForAny(...)
    local barrier_ctx = { co = coroutine.running() }

    local functions = table.pack(...)
    local threads = {}
    for i = 1, functions.n do
        local fn = functions[i]
        expect(i, fn, "function")
        threads[i] = {
            co = coroutine.create(function() return exception.try_barrier(barrier_ctx, fn) end),
            filter = nil,
        }
    end

    local count = functions.n
    if count < 1 then return 0 end

    local event = { n = 0 }
    while true do
        for i = 1, count do
            local thread = threads[i]
            if thread.filter == nil or thread.filter == event[1] or event[1] == "terminate" then
                local ok, param = coroutine.resume(thread.co, table.unpack(event, 1, event.n))
                M.resumes = M.resumes + 1
                if not ok then
                    error(exception.wrap_error(param, thread.co), 0)
                end

                if coroutine.status(thread.co) == "dead" then return i end

                thread.filter = param
            end
        end

        event = table.pack(os.pullEventRaw())
    end
end

function M.waitForAll(...)
    local barrier_ctx = { co = coroutine.running() }

    local can_spawn, threads, count = false, {}, 0

    local function spawn(fn, ...)
        expect(1, fn, "function")
        if not can_spawn then error("Cannot spawn new functions outside of waitForAll", 2) end

        threads[count + 1] = {
            co = coroutine.create(function(...) return exception.try_barrier(barrier_ctx, fn, ...) end),
            filter = nil,
            resume_with = table.pack(...),
        }
        count = count + 1
    end

    local functions = table.pack(...)
    can_spawn = true
    for i = 1, functions.n, 1 do
        local fn = functions[i]
        expect(i, fn, "function")
        spawn(fn, spawn)
    end
    can_spawn = false

    local event = { n = 0 }
    while true do
        local i = 1
        while i <= count do
            local thread = threads[i]

            local resume_with
            if thread.resume_with then
                resume_with = thread.resume_with
                thread.resume_with = nil
            elseif thread.filter == nil or thread.filter == event[1] or event[1] == "terminate" then
                resume_with = event
            end

            if resume_with then
                can_spawn = true
                local ok, param = coroutine.resume(thread.co, table.unpack(resume_with, 1, resume_with.n))
                M.resumes = M.resumes + 1
                can_spawn = false

                if not ok then
                    error(exception.wrap_error(param, thread.co), 0)
                end

                if coroutine.status(thread.co) == "dead" then
                    table.remove(threads, i)
                    i, count = i - 1, count - 1
                end

                thread.filter = param
            end

            i = i + 1
        end

        if count == 0 then return end

        event = table.pack(os.pullEventRaw())
    end
end

-- Deterministic helpers ---------------------------------------------------
local MOD = 2147483647
function M.mix(h, v)
    if type(v) == "number" then
        return (h * 31 + (v % MOD)) % MOD
    elseif type(v) == "string" then
        for i = 1, #v do h = (h * 31 + v:byte(i)) % MOD end
        return h
    elseif type(v) == "boolean" then
        return (h * 31 + (v and 7 or 3)) % MOD
    else
        return (h * 31 + 1) % MOD
    end
end

-- CPU work with a JIT-friendly loop plus some interpreter/C-library work.
function M.work(seed, n)
    local x = seed % MOD
    for i = 1, n do
        x = (x * 48271 + i) % MOD
    end
    local t = {}
    for i = 1, 64 do t[i] = (x + i * 7919) % 1000 end
    table.sort(t)
    local s = table.concat(t, ",", 1, 16)
    return M.mix(x, s)
end

-- Simple serializer (sorted keys) for result comparison.
local function ser(v, out)
    local tv = type(v)
    if tv == "table" then
        local keys = {}
        for k in pairs(v) do keys[#keys + 1] = k end
        table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
        out[#out + 1] = "{"
        for _, k in ipairs(keys) do
            out[#out + 1] = tostring(k) .. "="
            ser(v[k], out)
            out[#out + 1] = ","
        end
        out[#out + 1] = "}"
    else
        out[#out + 1] = tostring(v)
    end
end
function M.serialize(v) local out = {} ser(v, out) return table.concat(out) end

return M
