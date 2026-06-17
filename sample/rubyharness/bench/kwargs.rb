def box(x:, y:, z: 0)
  x * 100 + y * 10 + z
end
s = 0; i = 0
while i < 6_000_000
  s += box(x: i & 3, y: i & 7, z: i & 1)
  i += 1
end
p s
