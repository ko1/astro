def bench
  s = 0
  5.upto(100) { |x| s += x }
  100.downto(50) { |x| s -= x }
  0.step(200, 4) { |x| s += x }
  s
end

OUTER = 200_000
result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p result
