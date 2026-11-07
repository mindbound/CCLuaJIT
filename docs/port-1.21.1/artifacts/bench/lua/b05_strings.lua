-- strings: table.concat building, byte/char/sub loops, string.format, gmatch/gsub/find
local NAME = "strings"
local K = STRINGS_K or 1
local function run()
  local acc = 0
  for rep = 1, 30 * K do
    local parts = {}
    for i = 1, 1000 do parts[#parts + 1] = tostring(i) end
    acc = acc + #table.concat(parts, ",")
  end
  local text = string.rep("The quick brown fox jumps over the lazy dog. ", 40)
  for rep = 1, 20 * K do
    local out = {}
    for i = 1, #text do
      local b = string.byte(text, i)
      if b >= 97 and b <= 122 then b = (b - 97 + 13) % 26 + 97 end
      out[i] = string.char(b)
    end
    local s = table.concat(out)
    for i = 1, #s - 5, 3 do if s:sub(i, i + 4) == "gur d" then acc = acc + 1 end end
    acc = acc + string.byte(s, rep % #s + 1)
  end
  for i = 1, 20000 * K do
    acc = acc + #string.format("%5d|%-8s|%6.2f|%x", i, "item", i / 7, i)
  end
  local doc = string.rep("key1=value1; key22=value22; name=Steve; x=10;\n", 40)
  for rep = 1, 40 * K do
    local n = 0
    for k, v in string.gmatch(doc, "(%w+)=(%w+)") do n = n + #k + #v end
    local s2, c = string.gsub(doc, "%d+", function(d) return "<" .. d .. ">" end)
    local p, found = 1, 0
    while true do
      local a, b = string.find(doc, "name", p, true)
      if not a then break end
      found = found + 1; p = b + 1
    end
    acc = acc + n + c + found + #s2 + #(doc:upper():lower())
  end
  return string.format("%.0f", acc)
end
