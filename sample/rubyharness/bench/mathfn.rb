s = 0.0; i = 1
while i < 3_000_000
  s += Math.sqrt(i.to_f) + (i % 2 == 0 ? Math.sin(i.to_f) : Math.cos(i.to_f))
  i += 1
end
p s.round(4)
