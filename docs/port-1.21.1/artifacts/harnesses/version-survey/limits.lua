-- How many results can string.byte / unpack return? (AUKit and sanjuuni pick smaller chunks when `jit` is set)
local unpack = table.unpack or unpack
local function try(n, how)
  local s = string.rep("a", n)
  local ok, err = pcall(function()
    if how == "byte" then return #{s:byte(1, -1)} else local t = {s:byte(1, 7000)}; for i = #t + 1, n do t[i] = 97 end return #{unpack(t)} end
  end)
  return ok and "ok" or ("ERR " .. tostring(err):gsub("^.-: ", ""))
end
for _, how in ipairs({"byte", "unpack"}) do
  for _, n in ipairs({7680, 8000, 32768, 48000, 65000, 70000, 524288}) do
    local main = try(n, how)
    local co = coroutine.wrap(function() return try(n, how) end)()
    print(("%-6s n=%-7d main=%-40s coroutine=%s"):format(how, n, main, co))
  end
end
