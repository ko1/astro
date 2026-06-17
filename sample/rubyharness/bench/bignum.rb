total = 0; r = 0
1500.times do
  f = 1; i = 1
  while i < 500
    f = f * i
    i += 1
  end
  total += f % 1_000_000_007
end
p total
