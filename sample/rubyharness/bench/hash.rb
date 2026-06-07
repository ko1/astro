h = {}; i = 0
while i < 15_000_000
  h[i & 4095] = i
  i += 1
end
p h.size
