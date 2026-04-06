# Hash operations (insert, lookup)
h = {}
i = 0
while i < 100000
  h[i] = i * 2
  i += 1
end

sum = 0
i = 0
while i < 100000
  sum += h[i]
  i += 1
end
p(sum)
