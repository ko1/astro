def bench
  s = 0; i = 0
  while i < 4_000
    s += (1..20).map { |x| x + i }.select { |x| x.even? }.length
    i += 1
  end
  s
end

result = 0
i = 0
while i < 100
  result = bench
  i += 1
end
p result
