x = 10
if x > 5:
    print("big")
else:
    print("small")

y = 0
if y == 0:
    print("zero")
elif y > 0:
    print("pos")
else:
    print("neg")

n = 1
while n <= 5:
    print(n)
    n = n + 1

# nested if
def classify(v):
    if v < 0:
        return "neg"
    elif v == 0:
        return "zero"
    elif v < 10:
        return "small"
    else:
        return "big"

print(classify(-3))
print(classify(0))
print(classify(7))
print(classify(100))
