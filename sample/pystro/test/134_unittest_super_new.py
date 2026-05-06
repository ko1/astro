import unittest


class SuperNewTest(unittest.TestCase):
    def test_basic(self):
        class W:
            def __new__(cls, n):
                obj = super().__new__(cls)
                obj.n = n
                return obj
        w = W(5)
        self.assertEqual(w.n, 5)

    def test_inherited(self):
        class A:
            def __new__(cls, x):
                obj = super().__new__(cls)
                obj.x = x
                return obj
        class B(A):
            def __new__(cls, x, y):
                obj = super().__new__(cls, x)
                obj.y = y
                return obj
        b = B(1, 2)
        self.assertEqual(b.x, 1)
        self.assertEqual(b.y, 2)


unittest.main(globals())
