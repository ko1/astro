d = {}
for i in range(3000000):
    d[i] = i * 2
total = 0
for k in d.keys():
    total += d[k]
print(total)
