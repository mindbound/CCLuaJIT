-- Headless smoke test: runs as /startup.lua under the real bios + shell.
-- Every check is an assert, so any wrong result exits 1 (not only Lua errors).
local ok, err = pcall(function()
  print(os.version(), _HOST)
  headless.log("version=" .. os.version() .. " host=" .. tostring(_HOST))

  local t = os.clock(); sleep(0.2)                       -- os.startTimer + computer.tick() driven timers
  assert(os.clock() - t >= 0.15, "sleep(0.2) returned after " .. (os.clock() - t) .. " s")
  headless.log("sleep ok")

  local sum = headless.mainAdd(2, 3)                     -- MethodResult yield + MainThread + task_complete
  assert(sum == 5, "mainAdd(2,3) returned " .. tostring(sum))
  headless.log("mainAdd(2,3)=" .. sum)

  write("name? "); local line = read()                   -- char/key events queued from Java (--type Steve)
  assert(line == "Steve", "read() returned " .. tostring(line))
  headless.log("read() got: " .. line)

  local f = fs.open("out.txt", "w"); f.write("\255\128binary"); f.close()
  local h = fs.open("out.txt", "rb"); local s = h.readAll(); h.close()
  assert(s == "\255\128binary", "8-bit round trip returned " .. #s .. " bytes")   -- Latin-1, byte for byte
  headless.log("8-bit roundtrip ok len=" .. #s)

  local x = bit32.extract(0xF0, 4, 4)
  assert(x == 15, "bit32.extract(0xF0,4,4) returned " .. tostring(x))
  headless.log("bit32.extract(0xF0,4,4)=" .. x)

  assert(shell.run("ls", "rom"), "shell.run('ls', 'rom') failed")
  headless.log("programs run ok")
end)
if not ok then headless.log("FAIL: " .. tostring(err)); headless.exit(1) else headless.exit(0) end
