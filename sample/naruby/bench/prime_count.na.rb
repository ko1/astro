def prime_count(limit)
  count = 0
  n = 2
  while n < limit
    i = 2

    while i * i <= n && n % i != 0
      i += 1
    end
    
    # i += 1 while i * i <= n && n % i != 0

    count += 1 if i * i > n
    n += 1
  end
  count
end

i = 0
while i<100
  n = 100_000
  prime_count(n)
  i+=1
end
