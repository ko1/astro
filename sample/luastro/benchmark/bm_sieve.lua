-- Sieve of Eratosthenes
local N = 10000000
local primes = {}
for i = 2, N do primes[i] = true end
for i = 2, N do
  if primes[i] then
    for j = i*i, N, i do primes[j] = false end
  end
end
local cnt = 0
for i = 2, N do if primes[i] then cnt = cnt + 1 end end
print(cnt)
