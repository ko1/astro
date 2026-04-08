# Factorial (recursive, bignum)
def fact(n)
  if n < 2
    1
  else
    n * fact(n - 1)
  end
end

i = 0
while i < 160_000
  fact(50)
  i += 1
end
p(fact(50))
