"""Python で書いた range の equivalent — for-loop ターゲットが
C 内蔵 range ではなく user-iterator になる場合の AOT が効くかどうか。"""

class PyRange:
    def __init__(self, n):
        self.n = n
    def __iter__(self):
        self.i = 0
        return self
    def __next__(self):
        if self.i >= self.n:
            raise StopIteration
        v = self.i
        self.i += 1
        return v

s = 0
for i in PyRange(15000000):
    s += i
print(s)
