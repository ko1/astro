e = [1, 2, 3].each
r = []
loop { r << e.next }
p r
out = []
[10, 20].each.tap { |en| loop { out << en.next } }
p out
p (loop { break 42 })
