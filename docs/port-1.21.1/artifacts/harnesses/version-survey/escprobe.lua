-- Does the VM accept the invalid escape "\)" (used by vacovsky tk.lua:26 / Inksaver clsTurtle.lua:24 to find ")" in _HOST),
-- and what does find(")") return? Chunks are compiled from strings so a compile error is reported, not fatal.
local chunks = {
  { "escape", [[return ("x\)y")]] },
  { "find(')')", [[return ("ab)c"):find(")")]] },
  { "vacovsky tk.lua:26", [[local _HOST = "ComputerCraft 1.121.0 (Minecraft 1.21.1)"
     return tonumber(_HOST:sub(_HOST:find("Minecraft") + 10, _HOST:find("\)") -3))]] },
  { "vacovsky + (d)", [[local _HOST = "ComputerCraft 1.121.0 (Minecraft 1.21.1) (CCLuaJIT 0.1.0)"
     return tonumber(_HOST:sub(_HOST:find("Minecraft") + 10, _HOST:find("\)") -3))]] },
}
for _, c in ipairs(chunks) do
  local f, err = load(c[2], "=" .. c[1])
  if not f then print(c[1], "COMPILE ERROR", err) else print(c[1], pcall(f)) end
end
