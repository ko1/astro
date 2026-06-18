INNER = 100_000
OUTER = 120

def bench
  s = +""; i = 0
  while i < INNER
    s << "x"
    i += 1
  end
  s.length
end

result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p(result)
