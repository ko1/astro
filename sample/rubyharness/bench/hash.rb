INNER = 100_000
OUTER = 150

def bench
  h = {}; i = 0
  while i < INNER
    h[i & 4095] = i
    i += 1
  end
  h.size
end

result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p result
