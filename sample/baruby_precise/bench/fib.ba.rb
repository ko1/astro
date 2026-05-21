def fib n
  if n < 2
    1
  else
    fib(n-2) + fib(n-1)
  end
end

p fib(40)

