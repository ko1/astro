def ack(m, n)
  if m == 0 then n + 1
  elsif n == 0 then ack(m - 1, 1)
  else ack(m - 1, ack(m, n - 1))
  end
end

def bench = ack(2, 200)

result = 0
i = 0
while i < 400
  result = bench
  i += 1
end
p(result)
