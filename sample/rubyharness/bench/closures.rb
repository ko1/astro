def adder(n) = ->(x) { x + n }

def bench
  s = 0
  i = 0
  while i < 100_000
    f = adder(i)
    s += f.call(1)
    i += 1
  end
  s
end

result = 0
i = 0
while i < 30
  result = bench
  i += 1
end
p(result)
