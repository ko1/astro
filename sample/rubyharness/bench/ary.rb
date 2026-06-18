def bench
  a = []
  i = 0
  while i < 100_000
    a << (i & 1023)
    i += 1
  end
  a.sum
end

result = 0
i = 0
while i < 200
  result = bench
  i += 1
end
p(result)
