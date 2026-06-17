s = 0; i = 0
while i < 1_000_000
  s += (1..20).map { |x| x + i }.select { |x| x.even? }.length
  i += 1
end
p s
