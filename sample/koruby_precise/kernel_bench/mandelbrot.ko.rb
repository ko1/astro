# Mandelbrot — Float-heavy benchmark.
# All ivar-less, all heap Float (koruby has no FLONUM yet) so this
# stresses the Float allocation path and the dispatch_binop slow path
# for Float arithmetic.

def mandelbrot(w, h, max_iter)
  count = 0
  y = 0
  while y < h
    cy = (y.to_f / h) * 2.0 - 1.0
    x = 0
    while x < w
      cx = (x.to_f / w) * 3.5 - 2.5
      zx = 0.0
      zy = 0.0
      i = 0
      while i < max_iter
        zx2 = zx * zx
        zy2 = zy * zy
        break if zx2 + zy2 > 4.0
        zy = 2.0 * zx * zy + cy
        zx = zx2 - zy2 + cx
        i = i + 1
      end
      count = count + i
      x = x + 1
    end
    y = y + 1
  end
  count
end

t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
result = mandelbrot(80, 60, 100)
t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
puts "mandelbrot 80x60 max=100: #{((t1 - t0) * 1000).to_i} ms (count=#{result})"
