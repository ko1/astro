H = {}
i = 0
while i < 5000
  H[i] = i * 2
  i += 1
end

def bench
  s = 0
  H.each { |k, v| s += k + v }
  s
end

OUTER = 2000
result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p result
