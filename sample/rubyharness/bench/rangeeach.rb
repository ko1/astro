s = 0; j = 0
while j < 4000
  (1..2000).each { |x| s += x }
  j += 1
end
p s
