# Sieve of Eratosthenes (ChocoPy style: pre-sized list, no append).
n:int = 50
flags:[bool] = None
i:int = 0
j:int = 0
count:int = 0

flags = [True]
# grow to n+1 entries by concatenation
while len(flags) <= n:
    flags = flags + [True]

i = 2
while i * i <= n:
    if flags[i]:
        j = i * i
        while j <= n:
            flags[j] = False
            j = j + i
    i = i + 1

i = 2
while i <= n:
    if flags[i]:
        count = count + 1
    i = i + 1
print(count)
