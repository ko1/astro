# Hash operations (insert, lookup)
def bench
  h = {}
  i = 0
  while i < 5000
    h[i] = i * 2
    i += 1
  end

  sum = 0
  i = 0
  while i < 5000
    sum += h[i]
    i += 1
  end
  sum
end

result = 0
i = 0
while i < 1000
  result = bench
  i += 1
end
p(result)
