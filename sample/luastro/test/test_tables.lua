-- Table basics
local t = {10, 20, 30}
print(#t, t[1], t[2], t[3])

local h = {name = "x", value = 42}
print(h.name, h.value)
print(h["name"])

t[4] = 40
print(#t, t[4])

table.insert(t, 50)
print(#t, t[5])

table.insert(t, 1, 0)
print(t[1], t[2], t[3])

local removed = table.remove(t, 1)
print(removed, t[1])

print(table.concat({"a", "b", "c"}, "-"))

local mixed = {1, 2, 3, four = 4}
local sum = 0
for _, v in ipairs(mixed) do sum = sum + v end
print(sum)
