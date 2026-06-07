keep = nil; i = 0
while i < 8_000_000
  a = [i, i + 1, i + 2]
  keep = a if i & 65_535 == 0
  i += 1
end
p keep
