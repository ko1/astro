def bench
  s = 0; i = 0
  while i < 10_000
    str = format("%05d-%x:%s", i & 0xffff, i & 0xff, (i.even? ? "yes" : "no"))
    s += str.length
    i += 1
  end
  s
end

result = 0
i = 0
while i < 100
  result = bench
  i += 1
end
p result
