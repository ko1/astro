print("hello, luastro")
print(1 + 2 * 3)
print(2^10)

local function fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end
print(fib(10))

local t = {10, 20, 30, name = "x"}
print(#t, t[1], t[2], t.name)

for i = 1, 5 do io.write(i, " ") end
print()
