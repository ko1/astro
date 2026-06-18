INNER = 30_000
OUTER = 100

def bench
  s = 0.0; i = 1
  while i < INNER
    s += Math.sqrt(i.to_f) + (i % 2 == 0 ? Math.sin(i.to_f) : Math.cos(i.to_f))
    i += 1
  end
  s.round(4)
end

result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p result
