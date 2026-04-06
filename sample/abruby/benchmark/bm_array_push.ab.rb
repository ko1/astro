# Array push only
a = []
i = 0
while i < 200000
  a.push(i)
  i += 1
end
p(a.length)
