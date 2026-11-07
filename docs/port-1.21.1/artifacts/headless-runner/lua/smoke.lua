-- Headless smoke test: runs as /startup.lua under the real bios + shell.
local ok, err = pcall(function()
  print(os.version(), _HOST)
  headless.log("version=" .. os.version() .. " host=" .. tostring(_HOST))
  local t = os.clock(); sleep(0.2)                       -- os.startTimer + computer.tick() driven timers
  headless.log("sleep ok, clock delta>=0.2: " .. tostring(os.clock() - t >= 0.15))
  headless.log("mainAdd(2,3)=" .. headless.mainAdd(2, 3)) -- MethodResult yield + MainThread + task_complete
  write("name? "); local line = read()                   -- char/key events queued from Java
  headless.log("read() got: " .. tostring(line))
  local f = fs.open("out.txt", "w"); f.write("\255\128binary"); f.close()
  local h = fs.open("out.txt", "rb"); local s = h.readAll(); h.close()
  headless.log("8-bit roundtrip ok: " .. tostring(s == "\255\128binary") .. " len=" .. #s)
  headless.log("bit32.extract(0xF0,4,4)=" .. bit32.extract(0xF0, 4, 4))
  shell.run("ls", "rom")
  headless.log("programs run ok")
end)
if not ok then headless.log("FAIL: " .. tostring(err)); headless.exit(1) else headless.exit(0) end
