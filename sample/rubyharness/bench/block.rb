def bench
  s = 0
  (1..100_000).each { |i| s += i }
  s
end

result = 0
i = 0
while i < 150
  result = bench
  i += 1
end
p(result)
