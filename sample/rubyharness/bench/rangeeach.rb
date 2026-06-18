def bench
  s = 0
  (1..2000).each { |x| s += x }
  s
end

result = 0
j = 0
while j < 4000
  result = bench
  j += 1
end
p result
