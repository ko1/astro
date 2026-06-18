INNER = 100_000
OUTER = 120

def bench
  s = 0; i = 1
  while i < INNER
    s += (1_000_000 / i) + (1_000_003 % (i + 1))
    i += 1
  end
  s
end

result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p result
