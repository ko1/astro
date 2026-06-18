# GCD (Euclidean algorithm, deep recursion)
def gcd(a, b)
  if b == 0
    a
  else
    gcd(b, a % b)
  end
end

INNER = 50_000
OUTER = 120

def bench
  sum = 0
  i = 1
  while i <= INNER
    sum += gcd(i, i + 7)
    i += 1
  end
  sum
end

result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p result
