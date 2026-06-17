s = 0; i = 0
while i < 1_000_000
  str = format("%05d-%x:%s", i & 0xffff, i & 0xff, (i.even? ? "yes" : "no"))
  s += str.length
  i += 1
end
p s
