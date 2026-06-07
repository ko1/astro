a = []; x = 12_345; i = 0
while i < 2_500_000
  x = (x * 1_103_515_245 + 12_345) & 0x7fff_ffff
  a << x
  i += 1
end
s = a.sort
p [s.first, s.last, s.length]
