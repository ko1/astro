# Array operations (push, index, length)
def bench
  a = []
  i = 0
  while i < 15_000
    a.push(i)
    i += 1
  end

  sum = 0
  i = 0
  len = a.length
  while i < len
    sum += a[i]
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
