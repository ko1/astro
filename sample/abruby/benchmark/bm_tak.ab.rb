# Takeuchi function
def tak(x, y, z)
  if y < x
    tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y))
  else
    z
  end
end

i = 0
while i < 500
  tak(18, 12, 6)
  i += 1
end
p(tak(18, 12, 6))
