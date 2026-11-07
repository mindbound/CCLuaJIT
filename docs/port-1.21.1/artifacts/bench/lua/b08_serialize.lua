-- serialize: textutils.serialize-style recursive serializer + load()-based deserializer
local NAME = "serialize"
local REPS = SER_REPS or 10
local loadstr = loadstring or load
local function ser(t, tracking, indent, out)
  local ty = type(t)
  if ty == "table" then
    if tracking[t] then error("Cannot serialize table with recursive entries") end
    tracking[t] = true
    if next(t) == nil then
      out[#out + 1] = "{}"
    else
      out[#out + 1] = "{\n"
      local sub = indent .. "  "
      local seen = {}
      for k, v in ipairs(t) do
        seen[k] = true
        out[#out + 1] = sub; ser(v, tracking, sub, out); out[#out + 1] = ",\n"
      end
      for k, v in pairs(t) do
        if not seen[k] then
          if type(k) == "string" and string.match(k, "^[%a_][%a%d_]*$") then
            out[#out + 1] = sub .. k .. " = "
          else
            out[#out + 1] = sub .. "[ "; ser(k, tracking, sub, out); out[#out + 1] = " ] = "
          end
          ser(v, tracking, sub, out); out[#out + 1] = ",\n"
        end
      end
      out[#out + 1] = indent .. "}"
    end
    tracking[t] = nil
  elseif ty == "string" then
    out[#out + 1] = string.format("%q", t)
  else
    out[#out + 1] = tostring(t)
  end
end
local function serialize(t) local out = {}; ser(t, {}, "", out); return table.concat(out) end
local function unserialize(s) local f = assert(loadstr("return " .. s, "unserialize")); return f() end
local function walk(t, acc)
  for k, v in pairs(t) do
    local tv = type(v)
    if tv == "table" then walk(v, acc)
    elseif tv == "number" then acc[1] = acc[1] + v
    elseif tv == "string" then acc[2] = acc[2] + #v
    elseif tv == "boolean" then acc[3] = acc[3] + (v and 1 or 0) end
    acc[4] = acc[4] + 1
  end
end
local function makeData()
  local items = {}
  for i = 1, 300 do
    items[i] = {
      name = "minecraft:item_" .. (i % 37), count = i % 64 + 1, damage = (i % 9) * 0.5,
      tags = { ["minecraft:logs"] = (i % 3 == 0), ["c:ingots"] = (i % 5 == 0) },
      nbt = { display = { Name = "Item #" .. i .. " \"quoted\"" }, ench = { { id = "sharpness", lvl = i % 5 }, { id = "unbreaking", lvl = 3 } } },
      slots = { i, i + 1, i + 2, i + 3 },
    }
  end
  return { version = 3, owner = "steve", items = items, flags = { true, false, true } }
end
local DATA = makeData()
local function run()
  local acc = { 0, 0, 0, 0 }
  for r = 1, REPS do
    local s = serialize(DATA)
    local back = unserialize(s)
    walk(back, acc)
  end
  return string.format("%.1f %.0f %.0f %.0f", acc[1], acc[2], acc[3], acc[4])
end
