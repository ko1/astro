# Method call overhead
def incr(x)
  x + 1
end

i = 0
while i < 5000000
  i = incr(i)
end
p(i)
