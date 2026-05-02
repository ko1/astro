def my_gcd(a, b)
  while b != 0
    t = b
    b = a % b
    a = t
  end
  a
end

# Sustained scale: ~1 sec — my_gcd with non-trivial pair, iterated many times.
i = 0
acc = 0
while i < 50_000_000
  acc = acc + my_gcd(2_147_483_647, 1_073_741_823)
  i += 1
end
p acc
