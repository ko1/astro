# tight array index read/write (no allocation in the hot loop)
a = Array.new(1000) { |i| i }
s = 0; j = 0
while j < 20_000
  k = 0
  while k < 1000
    a[k] = a[k] + 1
    s += a[k]
    k += 1
  end
  j += 1
end
p s
