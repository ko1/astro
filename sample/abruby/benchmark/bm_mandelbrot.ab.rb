# Mandelbrot set (Float-heavy computation)
def mandelbrot(cr, ci, max_iter)
  zr = 0.0
  zi = 0.0
  i = 0
  result = max_iter
  while i < max_iter
    tr = zr * zr - zi * zi + cr
    zi = 2.0 * zr * zi + ci
    zr = tr
    if zr * zr + zi * zi > 4.0
      result = i
      i = max_iter  # break
    end
    i += 1
  end
  result
end

size = 80
max_iter = 50
sum = 0
y = 0
while y < size
  x = 0
  while x < size
    cr = (x * 3.0 / size) - 2.0
    ci = (y * 2.0 / size) - 1.0
    sum += mandelbrot(cr, ci, max_iter)
    x += 1
  end
  y += 1
end
p(sum)
