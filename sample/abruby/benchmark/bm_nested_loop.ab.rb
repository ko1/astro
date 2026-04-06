# Nested loop (integer arithmetic)
sum = 0
i = 0
while i < 1000
  j = 0
  while j < 1000
    sum += i * j
    j += 1
  end
  i += 1
end
p(sum)
