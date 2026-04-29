local function tak(x, y, z)
  if y < x then
    return tak(tak(x-1, y, z), tak(y-1, z, x), tak(z-1, x, y))
  else
    return z
  end
end
local r
for _ = 1, 100 do r = tak(22, 16, 8) end
print(r)
