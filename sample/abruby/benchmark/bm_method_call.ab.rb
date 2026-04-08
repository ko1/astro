# Method call overhead
def incr(x)
  x + 1
end

i = 0
while i < 50_000_000
  i = incr(i)
end
p(i)
