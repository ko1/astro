s = 0.0; i = 1
while i < 12_000_000
  s += 1.0 / (i * i)
  i += 1
end
p (s * 6.0).round(6)
