# Rich class behaviour: dunders, inheritance, MRO, descriptors.

# Basic class with __init__ / methods.
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def magnitude(self):
        return (self.x * self.x + self.y * self.y) ** 0.5
    def __repr__(self):
        return f"Point({self.x}, {self.y})"

p = Point(3, 4)
print(p.magnitude())
print(p)
print(repr(p))

# Inheritance.
class Animal:
    def __init__(self, name):
        self.name = name
    def greet(self):
        return f"Hi, I'm {self.name}"

class Dog(Animal):
    def greet(self):
        return Animal.greet(self) + " (woof)"

d = Dog("Rex")
print(d.greet())

# super().
class Cat(Animal):
    def __init__(self, name, color):
        super().__init__(name)
        self.color = color
    def greet(self):
        return super().greet() + f" — I'm {self.color}"

c = Cat("Whiskers", "black")
print(c.greet())

# Multiple inheritance with C3 MRO.
class A:
    def who(self): return "A"
class B(A):
    def who(self): return "B->" + super().who()
class C(A):
    def who(self): return "C->" + super().who()
class D(B, C):
    def who(self): return "D->" + super().who()

print(D().who())

# Class attributes.
class Config:
    DEFAULT = "default"
    count = 0
    def __init__(self):
        Config.count += 1
        self.name = Config.DEFAULT

a1 = Config(); a2 = Config(); a3 = Config()
print(Config.count)
print(a1.name, a2.name, a3.name)

# Operator dunders.
class Vec:
    def __init__(self, x, y):
        self.x = x; self.y = y
    def __add__(self, other):
        return Vec(self.x + other.x, self.y + other.y)
    def __sub__(self, other):
        return Vec(self.x - other.x, self.y - other.y)
    def __mul__(self, k):
        return Vec(self.x * k, self.y * k)
    def __eq__(self, other):
        return isinstance(other, Vec) and self.x == other.x and self.y == other.y
    def __repr__(self):
        return f"Vec({self.x}, {self.y})"

v = Vec(1, 2) + Vec(3, 4)
print(v)
print(Vec(5, 6) - Vec(1, 1))
print(Vec(1, 2) * 3)
print(Vec(1, 2) == Vec(1, 2))
print(Vec(1, 2) == Vec(1, 3))

# Comparison dunders.
class Box:
    def __init__(self, n): self.n = n
    def __lt__(self, o): return self.n < o.n
    def __le__(self, o): return self.n <= o.n
    def __eq__(self, o): return self.n == o.n
    def __repr__(self): return f"Box({self.n})"

b1 = Box(1); b2 = Box(2); b3 = Box(3)
print(b1 < b2)
print(b2 < b1)
print(b1 < b1)
print(b1 <= b1)
print(sorted([b3, b1, b2]))

# __len__, __getitem__, __setitem__.
class MyList:
    def __init__(self):
        self.data = []
    def __len__(self):
        return len(self.data)
    def __getitem__(self, i):
        return self.data[i]
    def __setitem__(self, i, v):
        self.data[i] = v
    def append(self, v):
        self.data.append(v)

ml = MyList()
ml.append(10)
ml.append(20)
ml.append(30)
print(len(ml))
print(ml[0])
ml[1] = 99
print(ml[1])

# Static / class method (decorators).
class M:
    @staticmethod
    def stat():
        return "static"
    @classmethod
    def kls(cls):
        return cls.__name__ if False else "cls-call"

print(M.stat())
print(M().stat())
print(M.kls())

# isinstance with multiple bases.
print(isinstance(d, Dog))
print(isinstance(d, Animal))
print(isinstance(d, Cat))
print(isinstance(d, (Cat, Dog)))
print(isinstance(d, (str, list)))

# issubclass.
print(issubclass(Dog, Animal))
print(issubclass(Cat, Animal))
print(issubclass(Animal, Dog))
print(issubclass(D, A))
print(issubclass(D, B))

# Attribute access via dot.
class O:
    pass
o = O()
o.x = 1
o.y = "hi"
print(o.x, o.y)
o.x = 99
print(o.x)
del o.y
try:
    print(o.y)
except AttributeError:
    print("noy")

# Class hierarchy attr lookup.
class Base:
    flag = "base"
class Sub(Base):
    pass
print(Sub.flag)
print(Sub().flag)
Sub.flag = "sub"
print(Sub.flag)
print(Base.flag)
