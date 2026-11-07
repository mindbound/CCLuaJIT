-- Differential check around the 4096-char scan chunks of patch 07
-- (max_expand and matchbalance scan in chunks and poll between them).
-- Only single find/match calls (anchored where a gsub would be quadratic).
local out = {}
local lens = {1, 2, 4094, 4095, 4096, 4097, 4098, 8191, 8192, 8193, 12288, 12289, 16385}
local pats = {"^a*", "^a*b", "^a*$", "^a*b?", "^[ab]*c", "^(a*)(.)", "^.*", "^.*b", "^a+b", "^.-$",
  "^%b()", "^%b()x", "^(%b())", "^%b()%b()", "^x*a*", "^[^b]*b", "^%a*%d", "^(a*)b(a*)$", "^[a(]*[)b]"}
local function ser(...) local t = {} for i = 1, select("#", ...) do t[i] = tostring((select(i, ...))) end return table.concat(t, ",") end
local cnt = 0
for _, n in ipairs(lens) do
  local ks = {0, 1, 2, n - 1, n, 4095, 4096, 4097, 8191, 8192, 8193, 12288, 12289}
  for _, k in ipairs(ks) do
    for _, base in ipairs({"a", "("}) do
      if k <= n then
        local t = {}
        for i = 1, n do t[i] = base end
        if k >= 1 then t[k] = (base == "(") and ")" or "b" end
        local s = table.concat(t)
        local s2 = s .. string.rep(")", n) .. "x"
        local s3 = s .. "b"
        for _, p in ipairs(pats) do
          cnt = cnt + 1
          out[#out + 1] = ser(n, k, base, p) .. ":" .. ser(string.find(s, p)) .. "|" .. ser(string.find(s2, p))
            .. "|" .. ser(string.find(s3, p)) .. "|" .. ser(string.match(s2, p))
        end
        -- Unanchored %b() over the whole string once (quadratic only in bad cases).
        out[#out + 1] = ser(n, k, base, "%b()") .. ":" .. ser(string.find(s2, "%b()")) .. "|" .. ser(select(2, string.gsub(s3, "a*", "")))
      end
    end
  end
end
io.write(cnt, " cases\n", table.concat(out, "\n"), "\n")
