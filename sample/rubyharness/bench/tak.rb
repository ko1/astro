# Takeuchi function (recursive)
def tak(x, y, z)
  if y < x
    tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y))
  else
    z
  end
end

def bench = tak(18, 12, 6)

result = 0
i = 0
while i < 720
  result = bench
  i += 1
end
p(result)
