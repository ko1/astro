# Mandelbrot escape-count.
def mandel(cx, cy, max_iter):
    x = 0.0
    y = 0.0
    i = 0
    while i < max_iter:
        x2 = x * x
        y2 = y * y
        if x2 + y2 > 4.0:
            return i
        y = 2.0 * x * y + cy
        x = x2 - y2 + cx
        i += 1
    return max_iter

def total(w, h, max_iter):
    s = 0
    for py in range(h):
        cy = (py - h / 2) * 4.0 / h
        for px in range(w):
            cx = (px - w / 2) * 4.0 / w
            s += mandel(cx, cy, max_iter)
    return s

print(total(300, 200, 1000))
