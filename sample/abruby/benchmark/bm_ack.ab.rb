# Ackermann function
def ack(m, n)
  if m == 0
    n + 1
  elsif n == 0
    ack(m - 1, 1)
  else
    ack(m - 1, ack(m, n - 1))
  end
end

i = 0
while i < 15
  ack(3, 7)
  i += 1
end
p(ack(3, 7))
