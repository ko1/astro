import unittest


class SuperSpreadTest(unittest.TestCase):
    def test_args(self):
        class A:
            def __init__(self, x, y): self.x, self.y = x, y
        class B(A):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.b = 99
        b = B(1, 2)
        self.assertEqual(b.x, 1)
        self.assertEqual(b.y, 2)
        self.assertEqual(b.b, 99)

    def test_kwargs_passthrough(self):
        class A:
            def __init__(self, x=10, y=20): self.x, self.y = x, y
        class B(A):
            def __init__(self, **kw):
                super().__init__(**kw)
        b = B(x=5)
        self.assertEqual(b.x, 5)
        self.assertEqual(b.y, 20)


unittest.main(globals())
