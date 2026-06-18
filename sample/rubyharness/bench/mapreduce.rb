def bench
  (1..50_000).map { |x| x * 2 }.select { |x| x % 3 == 0 }.reduce(0) { |a, b| a + b }
end

OUTER = 200
result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p result
