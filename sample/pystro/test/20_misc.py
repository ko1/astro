# multi-assign
a = b = c = 5
print(a, b, c)

x = y = []
x.append(1)
print(x, y)   # both are the same list

# walrus
xs = [1, 2, 3, 4, 5]
if (n := len(xs)) > 2:
    print("got", n)

# walrus in while-style scan
data = [None, 0, "", "found", "bar"]
i = 0
while (v := data[i]) == None or v == 0 or v == "":
    i += 1
print(v)

# slice assign
a = [1, 2, 3, 4, 5]
a[1:3] = [99, 88, 77]
print(a)
a[0:2] = []
print(a)
a[1:1] = [50, 60]
print(a)

# slice assign with step
b = [10, 20, 30, 40, 50]
b[::2] = [1, 2, 3]
print(b)

# for-else
for x in [1, 2, 3]:
    print(x)
else:
    print("for done")

for x in [1, 2, 3]:
    if x == 2:
        break
else:
    print("never reached")
print("after for-with-break")

# while-else
n = 0
while n < 3:
    n += 1
else:
    print("while done", n)

# while-else with break
n = 0
while n < 10:
    if n == 4:
        break
    n += 1
else:
    print("never reached")
print("after while-with-break", n)
