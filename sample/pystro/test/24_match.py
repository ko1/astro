def classify(x):
    match x:
        case 0:
            return "zero"
        case 1 | 2 | 3:
            return "small"
        case n if n < 10:
            return "single"
        case n if n < 100:
            return "double"
        case _:
            return "big"

print(classify(0))
print(classify(2))
print(classify(7))
print(classify(50))
print(classify(500))

# sequence pattern
def describe(s):
    match s:
        case []:
            return "empty"
        case [x]:
            return "one: " + str(x)
        case [a, b]:
            return "two: " + str(a) + "+" + str(b)
        case [a, b, c]:
            return "three: " + str(a + b + c)
        case _:
            return "many"

print(describe([]))
print(describe([5]))
print(describe([1, 2]))
print(describe([10, 20, 30]))
print(describe([1, 2, 3, 4]))

# class pattern (isinstance check)
class Empty: pass
class Filled:
    def __init__(self, v):
        self.v = v

def kind(x):
    match x:
        case Empty():
            return "empty"
        case Filled():
            return "filled"
        case _:
            return "other"

print(kind(Empty()))
print(kind(Filled(99)))
print(kind(42))

# nested
def deep(x):
    match x:
        case [1, [2, 3], 4]:
            return "matched"
        case _:
            return "no"

print(deep([1, [2, 3], 4]))
print(deep([1, [2, 4], 4]))
