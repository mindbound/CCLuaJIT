-- lzw: string-keyed dictionary compressor + decompressor (round-trip checked)
local NAME = "lzw"
local REPS = LZW_REPS or 4
local function compress(s)
  local dict = {}
  for i = 0, 255 do dict[string.char(i)] = i end
  local nextc, w, out = 256, "", {}
  for i = 1, #s do
    local c = string.sub(s, i, i)
    local wc = w .. c
    if dict[wc] then
      w = wc
    else
      out[#out + 1] = dict[w]
      dict[wc] = nextc; nextc = nextc + 1
      w = c
    end
  end
  if w ~= "" then out[#out + 1] = dict[w] end
  return out
end
local function decompress(codes)
  local dict = {}
  for i = 0, 255 do dict[i] = string.char(i) end
  local nextc = 256
  local w = dict[codes[1]]
  local out = { w }
  for i = 2, #codes do
    local k = codes[i]
    local entry = dict[k]
    if not entry then entry = w .. string.sub(w, 1, 1) end
    out[#out + 1] = entry
    dict[nextc] = w .. string.sub(entry, 1, 1); nextc = nextc + 1
    w = entry
  end
  return table.concat(out)
end
local INPUT
do
  local seed = 99
  local function rnd() seed = (seed * 16807) % 2147483647; return seed end
  local words = { "turtle", "redstone", "computer", "monitor", "modem", "peripheral", "inventory",
    "diamond", "cobblestone", "the", "a", "of", "and", "mining", "chest", "furnace" }
  local parts = {}
  for i = 1, 6000 do parts[i] = words[rnd() % #words + 1] end
  INPUT = table.concat(parts, " ")
end
local function run()
  local ncodes = 0
  for r = 1, REPS do
    local codes = compress(INPUT)
    local back = decompress(codes)
    assert(back == INPUT, "lzw roundtrip failed")
    ncodes = ncodes + #codes
  end
  return string.format("%.0f:%.0f", #INPUT, ncodes)
end
