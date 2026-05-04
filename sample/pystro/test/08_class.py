class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def manhattan(self):
        return abs(self.x) + abs(self.y)
    def add(self, other):
        return Point(self.x + other.x, self.y + other.y)

p = Point(3, -4)
print(p.x, p.y)
print(p.manhattan())

q = Point(1, 1)
r = p.add(q)
print(r.x, r.y)

# inheritance + base call (no super() yet, but base method visible)
class Animal:
    def __init__(self, name):
        self.name = name
    def kind(self):
        return "animal"

class Dog(Animal):
    def kind(self):
        return "dog"

a = Animal("a")
d = Dog("rex")
print(a.name, a.kind())
print(d.name, d.kind())
print(isinstance(d, Animal))
print(isinstance(d, Dog))
print(isinstance(a, Dog))

# class-level attribute lookup walks bases
class C(Animal):
    pass

c = C("cc")
print(c.kind())
print(c.name)
