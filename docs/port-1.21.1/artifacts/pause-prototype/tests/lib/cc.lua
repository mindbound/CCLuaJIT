-- The CC:T 1.121.0 code the tests drive, plus deterministic helpers. The CC:T code is os.pullEventRaw and
-- os.pullEvent from bios.lua, and parallel.waitForAny / waitForAll from rom/apis/parallel.lua. It is CCPL /
-- MPL-2.0, so this repository does not contain it: this file reads it at run time from the ROM directory named by
-- CCT_ROM, which ../../../extract_cct_rom.py fills from the CC:T core jar (see ../../README.md, "Build notes").
--
-- parallel.lua runs the way bios.lua's os.loadAPI runs an API: in its own environment that falls back to the global
-- table. Here that is a CC-like global table: the host's globals, plus os.pullEvent(Raw) from bios.lua and a
-- loadfile / dofile / fs.exists that read from CCT_ROM. That is enough for parallel.lua's tiny_require to load the
-- real cc.expect and cc.internal.exception. The host puts no native cc_try_barrier in the registry, so
-- cc.internal.exception stores its own Lua one there.

local M = {}

local HELP = ": see pause-prototype/README.md, \"Build notes\""
local ROM = os.getenv("CCT_ROM")
if not ROM or ROM == "" then error("CCT_ROM is not set" .. HELP, 0) end
ROM = ROM:gsub("[/\\]+$", "")

local function read_rom(path)
    local f = io.open(ROM .. "/" .. path, "rb")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end

-- The tests are written for CC:T 1.121.0's code. Its changelog names the version in the first line.
local changelog = read_rom("rom/help/changelog.md")
if not changelog then error("no CC:T ROM in CCT_ROM (" .. ROM .. ")" .. HELP, 0) end
if not changelog:find("^# New features in CC: Tweaked 1%.121%.0\r?\n") then
    error("the ROM in CCT_ROM (" .. ROM .. ") is not CC:T 1.121.0" .. HELP, 0)
end

-- The CC-like global table.
local cc_G = setmetatable({}, { __index = _G })
cc_G._G = cc_G
cc_G.fs = { exists = function(path) return read_rom((path:gsub("^/+", ""))) ~= nil end }

-- Like bios.lua's loadfile, this names the chunk "@/" .. path.
function cc_G.loadfile(path, mode, env)
    path = path:gsub("^/+", "")
    local src = read_rom(path)
    if not src then return nil, "File not found: " .. path .. " in CCT_ROM (" .. ROM .. ")" end
    return load(src, "@/" .. path, mode, env or cc_G)
end

function cc_G.dofile(path)
    local fn, err = cc_G.loadfile(path, nil, cc_G)
    if not fn then error(err .. HELP, 0) end
    return fn()
end

-- os.pullEventRaw and os.pullEvent: each definition is cut out of bios.lua and run in an environment whose `os` is
-- a fresh table. Newlines in front keep bios.lua's line numbers in error positions.
local bios = read_rom("bios.lua")
if not bios then error("no bios.lua in CCT_ROM (" .. ROM .. ")" .. HELP, 0) end
local bios_env = setmetatable({ os = {} }, { __index = cc_G })
for _, name in ipairs({ "pullEventRaw", "pullEvent" }) do
    local first, last = bios:find("\nfunction os%." .. name .. "%(.-\nend\r?\n")
    if not first then error("bios.lua in CCT_ROM (" .. ROM .. ") has no os." .. name .. HELP, 0) end
    local _, line = bios:sub(1, first):gsub("\n", "")
    assert(load(("\n"):rep(line) .. bios:sub(first + 1, last), "@/bios.lua", "t", bios_env))()
end
M.pullEventRaw, M.pullEvent = bios_env.os.pullEventRaw, bios_env.os.pullEvent
cc_G.os = setmetatable({ pullEventRaw = M.pullEventRaw, pullEvent = M.pullEvent }, { __index = os })

-- Instrumentation: the number of coroutine.resume calls inside parallel that returned. parallel.lua finds
-- `coroutine` in its API environment, so that table counts them and passes everything else through. It looks the
-- host's coroutine library up on each call, because the pause protocol replaces coroutine.resume.
M.resumes = 0
local function count(...)
    M.resumes = M.resumes + 1
    return ...
end
local api_env = setmetatable({
    coroutine = setmetatable({
        resume = function(...) return count(coroutine.resume(...)) end,
    }, { __index = function(_, k) return coroutine[k] end }),
}, { __index = cc_G })
local parallel, err = cc_G.loadfile("rom/apis/parallel.lua", nil, api_env)
if not parallel then error(err .. HELP, 0) end
parallel()
M.waitForAny, M.waitForAll = api_env.waitForAny, api_env.waitForAll

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
