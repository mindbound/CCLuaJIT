-- tables: array fill/scan, table.sort (default + comparator), table.insert/remove, word-count hash map
local NAME = "tables"
local K = TABLES_K or 1
local function run()
  local seed = 42
  local function rnd() seed = (seed * 16807) % 2147483647; return seed end
  local acc = 0
  -- array fill + scan
  for rep = 1, 20 * K do
    local t = {}
    for i = 1, 10000 do t[i] = i * 2 end
    for i = 1, #t do acc = acc + t[i] end
  end
  -- sort numbers
  local NS = 20000 * K
  local arr = {}
  for i = 1, NS do arr[i] = rnd() % 100000 end
  table.sort(arr)
  -- sort records with comparator
  local recs = {}
  for i = 1, NS / 2 do recs[i] = { k = rnd() % 1000, v = i } end
  table.sort(recs, function(a, b) if a.k ~= b.k then return a.k < b.k end return a.v < b.v end)
  acc = acc + arr[1] + arr[NS] + recs[1].v + recs[#recs].v
  -- stack push/pop via table.insert/remove
  local st = {}
  for i = 1, 30000 * K do table.insert(st, i) end
  for i = 1, 30000 * K do acc = acc + table.remove(st) end
  -- small queue with front insertion
  local q = {}
  for i = 1, 20000 * K do
    table.insert(q, 1, i)
    if #q > 32 then acc = acc + table.remove(q) end
  end
  -- word count hash map
  local syll = { "ka", "lo", "mi", "ne", "ru", "to", "sa", "vi", "po", "de" }
  local counts = {}
  for i = 1, 40000 * K do
    local n = rnd() % 3 + 1
    local w = syll[rnd() % 10 + 1]
    for j = 2, n do w = w .. syll[rnd() % 10 + 1] end
    counts[w] = (counts[w] or 0) + 1
  end
  local distinct, maxc = 0, 0
  for _, v in pairs(counts) do distinct = distinct + 1; if v > maxc then maxc = v end end
  return string.format("%.0f %.0f %.0f", acc, distinct, maxc)
end
