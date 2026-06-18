# Method call overhead
def incr(x)
  x + 1
end

def bench
  i = 0
  while i < 50_000
    i = incr(i)
  end
  i
end

result = 0
i = 0
while i < 1000
  result = bench
  i += 1
end
p(result)
