# Nested loop (integer arithmetic)
def bench
  sum = 0
  i = 0
  while i < 200
    j = 0
    while j < 200
      sum += i * j
      j += 1
    end
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
