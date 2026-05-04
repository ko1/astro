# super() __init__ chain
class Animal:
    def __init__(self, name):
        self.name = name
    def kind(self):
        return "animal"
    def describe(self):
        return self.name + ": " + self.kind()

class Dog(Animal):
    def __init__(self, name, breed):
        super().__init__(name)
        self.breed = breed
    def kind(self):
        return "dog (" + self.breed + ")"

class GoldenRetriever(Dog):
    def __init__(self, name):
        super().__init__(name, "golden retriever")
    def kind(self):
        return super().kind() + ", friendly"

a = Animal("kitty")
print(a.describe())

d = Dog("rex", "shepherd")
print(d.describe())

g = GoldenRetriever("buddy")
print(g.describe())

# arithmetic dunders
class Vec:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def __add__(self, other):
        return Vec(self.x + other.x, self.y + other.y)
    def __sub__(self, other):
        return Vec(self.x - other.x, self.y - other.y)
    def __mul__(self, k):
        return Vec(self.x * k, self.y * k)
    def __eq__(self, other):
        return self.x == other.x and self.y == other.y
    def __repr__(self):
        return "Vec(" + str(self.x) + "," + str(self.y) + ")"
    def __len__(self):
        return 2

a = Vec(1, 2)
b = Vec(10, 20)
print(a)
print(a + b)
print(b - a)
print(a * 3)
print(a == Vec(1, 2))
print(a == b)
print(len(a))
print([a, b])

# __getitem__ / __setitem__
class IntPair:
    def __init__(self):
        self.data = [0, 0]
    def __getitem__(self, i):
        return self.data[i]
    def __setitem__(self, i, v):
        self.data[i] = v

p = IntPair()
p[0] = 99
p[1] = 100
print(p[0], p[1])
