TEXT = ("the quick brown fox jumps " * 200)
INNER = 100
OUTER = 80

def bench
  total = 0; i = 0
  while i < INNER
    total += TEXT.split.length
    i += 1
  end
  total
end

result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p(result)
