# Nested loop (integer arithmetic)
sum = 0
i = 0
while i < 10_000
  j = 0
  while j < 10_000
    sum += i * j
    j += 1
  end
  i += 1
end
p(sum)
