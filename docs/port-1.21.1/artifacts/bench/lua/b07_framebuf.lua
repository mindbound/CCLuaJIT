-- framebuf: 51x19 text/fg/bg line buffers updated with sub/rep/concat (like CC window API / UI libs)
local NAME = "framebuf"
local FRAMES = FB_FRAMES or 2000
local W, H = 51, 19
local hex = "0123456789abcdef"
local function run()
  local seed = 7
  local function rnd() seed = (seed * 16807) % 2147483647; return seed end
  local text, fg, bg = {}, {}, {}
  local emptyT = string.rep(" ", W)
  local blitsum = 0
  for frame = 1, FRAMES do
    local ci = frame % 16 + 1
    local emptyBG, emptyFG = string.rep(hex:sub(ci, ci), W), string.rep("0", W)
    for y = 1, H do text[y] = emptyT; fg[y] = emptyFG; bg[y] = emptyBG end
    -- filled boxes (paintutils.drawFilledBox -> window.blit per line)
    for r = 1, 8 do
      local x1, y1 = rnd() % W + 1, rnd() % H + 1
      local x2, y2 = math.min(W, x1 + rnd() % 20), math.min(H, y1 + rnd() % 8)
      local c = hex:sub(r % 16 + 1, r % 16 + 1)
      local len = x2 - x1 + 1
      local fill, spaces = string.rep(c, len), string.rep(" ", len)
      for y = y1, y2 do
        local line = bg[y]; bg[y] = line:sub(1, x1 - 1) .. fill .. line:sub(x2 + 1)
        line = text[y]; text[y] = line:sub(1, x1 - 1) .. spaces .. line:sub(x2 + 1)
      end
    end
    -- text writes (term.write -> window line splice)
    for t = 1, 12 do
      local s = "Item " .. t .. ": " .. (frame * t)
      local x, y = rnd() % W + 1, rnd() % H + 1
      local n = math.min(#s, W - x + 1)
      local fc = hex:sub(t % 16 + 1, t % 16 + 1)
      local line = text[y]; text[y] = line:sub(1, x - 1) .. s:sub(1, n) .. line:sub(x + n)
      line = fg[y]; fg[y] = line:sub(1, x - 1) .. string.rep(fc, n) .. line:sub(x + n)
    end
    -- redraw: would call term.blit(text[y], fg[y], bg[y]) per line
    for y = 1, H do
      local tl, fl, bl = text[y], fg[y], bg[y]
      blitsum = blitsum + #tl + #fl + string.byte(bl, (frame + y) % W + 1) + string.byte(tl, y)
    end
  end
  return string.format("%.0f", blitsum)
end
