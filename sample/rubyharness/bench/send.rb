def inc(x) = x + 1
x = 0; i = 0
while i < 30_000_000
  x = inc(x)
  i += 1
end
p x
