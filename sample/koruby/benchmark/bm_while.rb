# Simple while loop (measures loop + fixnum add overhead)
def bench
  i = 0
  while i < 130_000
    i += 1
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
