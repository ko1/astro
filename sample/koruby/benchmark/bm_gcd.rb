# GCD (Euclidean algorithm, deep recursion)
def gcd(a, b)
  if b == 0
    a
  else
    gcd(b, a % b)
  end
end

sum = 0
i = 1
while i <= 6_000_000
  sum += gcd(i, i + 7)
  i += 1
end
p(sum)
