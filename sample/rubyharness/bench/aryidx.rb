# tight array index read/write (no allocation in the hot loop)
def bench
  a = Array.new(1000) { |i| i }
  s = 0
  k = 0
  while k < 1000
    a[k] = a[k] + 1
    s += a[k]
    k += 1
  end
  s
end

result = 0
i = 0
while i < 200
  result = bench
  i += 1
end
p(result)
