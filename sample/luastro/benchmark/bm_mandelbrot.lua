-- Mandelbrot set count (a smaller version of the classic kernel).
local W, H = 200, 200
local count = 0
for py = 0, H - 1 do
  for px = 0, W - 1 do
    local x0 = -2 + 3 * px / W
    local y0 = -1 + 2 * py / H
    local x, y = 0.0, 0.0
    local i = 0
    while x*x + y*y < 4 and i < 100 do
      local xn = x*x - y*y + x0
      y = 2*x*y + y0
      x = xn
      i = i + 1
    end
    if i == 100 then count = count + 1 end
  end
end
print(count)
