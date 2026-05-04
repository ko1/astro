# N-queens count
def queens(n):
    def safe(q, c):
        for i in range(len(q)):
            r = q[i]
            if r == c or abs(r - c) == len(q) - i:
                return False
        return True
    def go(q):
        if len(q) == n:
            return 1
        total = 0
        for c in range(n):
            if safe(q, c):
                total += go(q + [c])
        return total
    return go([])

print(queens(11))
