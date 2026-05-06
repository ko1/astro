import unittest


class UnaryPosTest(unittest.TestCase):
    def test_pos(self):
        class P:
            def __init__(self, v): self.v = v
            def __pos__(self): return P(self.v + 1)
            def __eq__(self, o): return isinstance(o, P) and self.v == o.v
            def __hash__(self): return self.v
        self.assertEqual(+P(5), P(6))

    def test_pos_pass_through(self):
        # Plain numbers keep value.
        self.assertEqual(+42, 42)
        self.assertEqual(+3.14, 3.14)


class CustomDivmodTest(unittest.TestCase):
    def test_dispatch(self):
        class N:
            def __init__(self, v): self.v = v
            def __divmod__(self, other):
                return (self.v // other, self.v % other)
        self.assertEqual(divmod(N(17), 5), (3, 2))


unittest.main(globals())
