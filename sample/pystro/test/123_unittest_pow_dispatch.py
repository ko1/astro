import unittest


class PowDispatchTest(unittest.TestCase):
    def test_user_pow(self):
        class N:
            def __init__(self, v): self.v = v
            def __pow__(self, n): return self.v ** n
        self.assertEqual(N(2) ** 10, 1024)

    def test_user_pow_with_mod(self):
        class N:
            def __init__(self, v): self.v = v
            def __pow__(self, n, mod=None):
                return pow(self.v, n) if mod is None else pow(self.v, n, mod)
        self.assertEqual(pow(N(7), 100, 13), 9)


unittest.main(globals())
