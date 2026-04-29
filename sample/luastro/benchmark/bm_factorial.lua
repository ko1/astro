local function fact(n)
  local r = 1
  for i = 2, n do r = r * i end
  return r
end
local total = 0
for i = 1, 10000000 do total = total + fact(20) end
print(total)
