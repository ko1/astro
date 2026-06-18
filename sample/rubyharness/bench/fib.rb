def fib(n) = n < 2 ? n : fib(n - 1) + fib(n - 2)

def bench = fib(24)

result = 0
i = 0
while i < 150
  result = bench
  i += 1
end
p(result)
