INNER = 100_000
OUTER = 120

def bench
  s = 0.0; i = 1
  while i < INNER
    s += 1.0 / (i * i)
    i += 1
  end
  (s * 6.0).round(6)
end

result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p result
