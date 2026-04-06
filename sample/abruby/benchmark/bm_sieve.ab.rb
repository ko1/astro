# Sieve of Eratosthenes (array-heavy)
n = 30000
sieve = []
i = 0
while i <= n
  sieve.push(true)
  i += 1
end
sieve[0] = false
sieve[1] = false

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

count = 0
i = 0
while i <= n
  if sieve[i]
    count += 1
  end
  i += 1
end
p(count)
