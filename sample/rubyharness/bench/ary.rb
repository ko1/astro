a = []; i = 0
while i < 20_000_000
  a << (i & 1023)
  i += 1
end
p a.sum
