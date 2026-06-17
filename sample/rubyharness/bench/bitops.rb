x = 0x1234_5678; s = 0; i = 0
while i < 15_000_000
  x = ((x << 1) | (x >> 31)) & 0xffff_ffff
  x ^= i & 0xff
  s += (x & 1) + ((x >> 8) & 1)
  i += 1
end
p [x, s]
