def bench
  a = []; x = 12_345; i = 0
  while i < 25_000
    x = (x * 1_103_515_245 + 12_345) & 0x7fff_ffff
    a << x
    i += 1
  end
  s = a.sort
  [s.first, s.last, s.length]
end

result = nil
i = 0
while i < 100
  result = bench
  i += 1
end
p result
