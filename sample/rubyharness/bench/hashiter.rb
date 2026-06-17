h = {}
i = 0
while i < 5000
  h[i] = i * 2
  i += 1
end
s = 0; j = 0
while j < 2000
  h.each { |k, v| s += k + v }
  j += 1
end
p s
