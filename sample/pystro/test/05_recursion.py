# Mutual recursion
def is_even(n):
    if n == 0:
        return True
    return is_odd(n - 1)

def is_odd(n):
    if n == 0:
        return False
    return is_even(n - 1)

print(is_even(10))
print(is_even(11))
print(is_odd(7))
print(is_odd(8))

# Ackermann (small).  This pushes the C stack a bit.
def ack(m, n):
    if m == 0:
        return n + 1
    if n == 0:
        return ack(m - 1, 1)
    return ack(m - 1, ack(m, n - 1))

print(ack(2, 3))
print(ack(3, 3))
