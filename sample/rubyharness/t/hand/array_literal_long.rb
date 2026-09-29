# Long array literals are built in chunks (build_array); mail's Ragel tables have 229k elements and
# overflowed the C stack.  Splats and a trailing **h inside / across chunk borders.
src = "[" + (0...600).map { |i| i % 97 == 0 ? "*[#{i}, -#{i}]" : i.to_s }.join(", ") + "]"
a = eval(src)
p a.size, a.sum, a[0, 5], a[-3..], a.frozen?
src2 = "def f(*r, **k) = [r.size, r.sum, k]\np f(" + (0...300).map(&:to_s).join(", ") + ", z: 1)\np f(" + (0...257).map(&:to_s).join(", ") + ", **{})"
eval(src2)
b = eval("[" + (1..5000).map { |i| (i * 7) % 1000 }.join(",") + "]")
p b.size, b.sum, b[256], b[257], b.last
p [*(1..3), 4].size, eval("[" + ("1," * 256) + "*[2, 3]]").size, eval("[" + ("1," * 512) + "]").size
