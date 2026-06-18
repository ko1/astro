def box(x:, y:, z: 0)
  x * 100 + y * 10 + z
end

INNER = 60_000
OUTER = 100

def bench
  s = 0; i = 0
  while i < INNER
    s += box(x: i & 3, y: i & 7, z: i & 1)
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
