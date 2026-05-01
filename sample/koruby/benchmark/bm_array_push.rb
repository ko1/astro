# Array push only
def bench
  a = []
  i = 0
  while i < 22_000
    a.push(i)
    i += 1
  end
  a.length
end

result = 0
i = 0
while i < 1000
  result = bench
  i += 1
end
p(result)
