s = 0; i = 0
while i < 200
  s += (1..50_000).map { |x| x * 2 }.select { |x| x % 3 == 0 }.reduce(0) { |a, b| a + b }
  i += 1
end
p s
