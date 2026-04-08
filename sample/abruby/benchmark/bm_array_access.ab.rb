# Array index access only
a = []
i = 0
while i < 20_000_000
  a.push(i)
  i += 1
end

sum = 0
i = 0
len = a.length
while i < len
  sum += a[i]
  i += 1
end
p(sum)
