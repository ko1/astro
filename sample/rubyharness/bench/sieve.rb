n = 5_000_000
sieve = Array.new(n + 1, true)
sieve[0] = sieve[1] = false
i = 2
while i * i <= n
  if sieve[i]
    j = i * i
    while j <= n
      sieve[j] = false
      j += i
    end
  end
  i += 1
end
count = 0; k = 0
while k <= n
  count += 1 if sieve[k]
  k += 1
end
p count
