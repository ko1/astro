-- Functions, closures, recursion
local function add(a, b) return a + b end
print(add(2, 3))

-- Closures
local function counter()
  local n = 0
  return function() n = n + 1; return n end
end
local c = counter()
print(c(), c(), c())

-- Recursion
local function fact(n)
  if n <= 1 then return 1 end
  return n * fact(n - 1)
end
print(fact(5), fact(10))

-- Multiple values
local function two() return 1, 2 end
local a, b = two()
print(a, b)

-- Varargs (skip — current implementation supports them but format here is fragile)
