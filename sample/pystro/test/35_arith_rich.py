# Numeric tower: int / bignum / float interaction edge cases.

# Fixnum overflow into bignum.
big = 1
for _ in range(100):
    big *= 2
print(big)
print(big > 0)
print(big * 2 // 2 == big)

# Mixed int / float.
print(1 + 2.5)
print(1.0 + 2)
print(round(10 / 3, 4))
print(10 // 3)
print(10 % 3)
print(-10 // 3)
print(-10 % 3)

# Float division.
print(1.0 / 4)
print(7.5 / 2.5)

# Negative powers.
print(2 ** 10)
print(2 ** 30)
print(2 ** 60)
print(2 ** 64)
print(2 ** 100)
print(2.0 ** -3)

# Bitwise.
print(0xFF & 0x0F)
print(0xF0 | 0x0F)
print(0xFF ^ 0x0F)
print(~0xFF)
print(1 << 10)
print(1024 >> 2)

# Comparisons with bignum.
print((1 << 100) > (1 << 99))
print((1 << 100) - (1 << 100) == 0)
print((1 << 100) % 7)

# Unary operators.
print(-5)
print(+5)
print(--5)
print(-(-5))

# Float specials.
import math
print(math.isclose(0.1 + 0.2, 0.3))
print(1.0 + 0.0 == 1.0)
print(-0.0 == 0.0)

# Division by zero raises.
try:
    print(1 / 0)
except ZeroDivisionError:
    print("zero1")
try:
    print(1 // 0)
except ZeroDivisionError:
    print("zero2")
try:
    print(1 % 0)
except ZeroDivisionError:
    print("zero3")

# Big factorial.
def fact(n):
    r = 1
    for i in range(1, n+1):
        r *= i
    return r
print(fact(20))
print(fact(50))
print(len(str(fact(100))))

# bool is int.
print(True + True)
print(True * 5)
print(False * 100)

# round (avoid .5 cases — Python uses banker's rounding).
print(round(2.7))
print(round(2.3))
print(round(-2.7))
print(round(-2.3))
print(round(3.14159, 2))
print(round(3.14159, 4))

# abs.
print(abs(-5))
print(abs(5))
print(abs(-3.14))
print(abs(- (1 << 100)))
