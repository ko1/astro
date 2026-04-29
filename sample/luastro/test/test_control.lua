-- Control flow
local x = 5
if x > 0 then print("positive") end
if x < 0 then print("negative") else print("not negative") end
if x == 0 then print("zero") elseif x > 0 then print("pos") else print("neg") end

-- while + break
local i = 1
while true do
  if i >= 4 then break end
  print(i)
  i = i + 1
end

-- repeat-until
local j = 1
repeat
  print("j=" .. j)
  j = j + 1
until j > 3

-- numeric for
for k = 1, 3 do print("k=" .. k) end
for k = 10, 1, -2 do print("dn=" .. k) end

-- generic for via ipairs
for i, v in ipairs({"a", "b", "c"}) do print(i, v) end
