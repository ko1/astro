# Adapted from CPython test_class.py — basic class behavior.

import unittest


class A:
    def __init__(self, n):
        self.n = n
    def double(self):
        return self.n * 2


class B(A):
    def double(self):
        return super().double() + 1


class C:
    x = "class-attr"
    count = 0
    def __init__(self):
        C.count = C.count + 1


class P:
    def __init__(self, x, y):
        self.x = x; self.y = y
    def __eq__(self, other):
        return isinstance(other, P) and self.x == other.x and self.y == other.y
    def __repr__(self):
        return "P(" + str(self.x) + "," + str(self.y) + ")"
    def __add__(self, other):
        return P(self.x + other.x, self.y + other.y)
    def __hash__(self):
        return self.x * 1000 + self.y


class Adder:
    def __init__(self, n): self.n = n
    def __call__(self, x): return self.n + x


class ClassTest(unittest.TestCase):
    def test_basic(self):
        a = A(5)
        self.assertEqual(a.n, 5)
        self.assertEqual(a.double(), 10)

    def test_inherit(self):
        b = B(3)
        self.assertEqual(b.double(), 7)
        self.assertIsInstance(b, A)
        self.assertIsInstance(b, B)

    def test_class_attr(self):
        self.assertEqual(C.x, "class-attr")
        c = C()
        self.assertEqual(c.x, "class-attr")

    def test_counter(self):
        # Each instantiation bumps C.count.  Reset first to a known
        # value because earlier tests may have created instances.
        C.count = 0
        a = C(); b = C(); d = C()
        self.assertEqual(C.count, 3)

    def test_dunders(self):
        p = P(1, 2)
        q = P(1, 2)
        r = P(2, 3)
        self.assertEqual(p, q)
        self.assertNotEqual(p, r)
        self.assertEqual(p + r, P(3, 5))
        self.assertEqual(repr(p), "P(1,2)")
        self.assertEqual(hash(p), hash(P(1, 2)))

    def test_dict_key(self):
        d = {P(1, 2): "a", P(3, 4): "b"}
        self.assertEqual(d[P(1, 2)], "a")
        self.assertEqual(d[P(3, 4)], "b")

    def test_call(self):
        inc = Adder(5)
        self.assertEqual(inc(10), 15)
        self.assertEqual(inc(100), 105)

    def test_isinstance_with_tuple(self):
        a = A(1)
        self.assertTrue(isinstance(a, A))
        self.assertTrue(isinstance(a, (str, A)))
        self.assertFalse(isinstance(a, (str, int)))

    def test_issubclass(self):
        self.assertTrue(issubclass(B, A))
        self.assertTrue(issubclass(B, B))
        self.assertFalse(issubclass(A, B))
        self.assertTrue(issubclass(B, (A, str)))

    def test_attr_dynamic(self):
        a = A(1)
        a.extra = "hello"
        self.assertEqual(a.extra, "hello")
        self.assertTrue(hasattr(a, "extra"))
        del a.extra
        self.assertFalse(hasattr(a, "extra"))

    def test_super_super(self):
        # Three-level inheritance with super chain.
        class X:
            def f(self): return ["X"]
        class Y(X):
            def f(self): return super().f() + ["Y"]
        class Z(Y):
            def f(self): return super().f() + ["Z"]
        self.assertEqual(Z().f(), ["X", "Y", "Z"])


unittest.main(globals())
