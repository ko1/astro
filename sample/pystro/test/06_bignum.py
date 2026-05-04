# GMP bignum
print(2 ** 64)
print(2 ** 100)
print(10 ** 20 + 1)
print(99999999999999999 * 99999999999999999)
print(2 ** 100 // (2 ** 50))
print(-(2 ** 70))

# overflow into bignum
x = 10 ** 18
y = 10 ** 18
print(x * y)

# back to fixnum
print(2 ** 100 - 2 ** 100)
print((2 ** 100) // (2 ** 100))

# bit ops
print(0xFF & 0x0F)
print(0xF0 | 0x0F)
print(0xFF ^ 0xF0)
print(1 << 10)
print(1024 >> 5)
print(~0)
print(~5)

# true div / floor div
print(7 / 2)
print(-7 // 2)
print(-7 % 3)
print(7 % -3)
