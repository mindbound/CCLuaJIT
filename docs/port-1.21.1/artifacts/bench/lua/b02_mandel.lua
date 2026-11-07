-- mandelbrot: float numeric loops
local NAME = "mandel"
local W, H, MAXI = 320, 240, MANDEL_I or 100
local function run()
  local inside, total = 0, 0
  for py = 0, H - 1 do
    local ci = (py / H) * 2.4 - 1.2
    for px = 0, W - 1 do
      local cr = (px / W) * 3.2 - 2.2
      local zr, zi, i = 0.0, 0.0, 0
      while i < MAXI do
        local zr2, zi2 = zr * zr, zi * zi
        if zr2 + zi2 > 4.0 then break end
        zi = 2.0 * zr * zi + ci
        zr = zr2 - zi2 + cr
        i = i + 1
      end
      total = total + i
      if i == MAXI then inside = inside + 1 end
    end
  end
  return string.format("%.0f/%.0f", inside, total)
end
