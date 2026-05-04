s = "the quick brown fox jumps over the lazy dog"
n = 0
for i in range(2000000):
    parts = s.split()
    n += len(parts)
print(n)
