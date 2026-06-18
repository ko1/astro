def inc(x) = x + 1

def bench
  x = 0; i = 0
  while i < 100_000
    x = inc(x)
    i += 1
  end
  x
end

result = 0
i = 0
while i < 300
  result = bench
  i += 1
end
p result
