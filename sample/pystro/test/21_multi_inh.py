class A:
    def hello(self):
        return "A.hello"
    def shared(self):
        return "A.shared"

class B:
    def world(self):
        return "B.world"
    def shared(self):
        return "B.shared"

class C(A, B):
    pass

c = C()
print(c.hello())
print(c.world())
print(c.shared())   # A's shared first (DFS)
print(isinstance(c, A))
print(isinstance(c, B))
print(isinstance(c, C))

# subclass overrides one
class D(A, B):
    def hello(self):
        return "D.hello"

d = D()
print(d.hello())
print(d.world())

# 3 bases
class M1:
    def m1(self): return "M1"
class M2:
    def m2(self): return "M2"
class M3:
    def m3(self): return "M3"

class All(M1, M2, M3):
    pass

a = All()
print(a.m1(), a.m2(), a.m3())

# super() with leftmost base
class P:
    def greet(self):
        return "hello"
class Q(P):
    def greet(self):
        return super().greet() + ", " + "world"

print(Q().greet())
