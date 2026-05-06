import unittest


class SliceClassTest(unittest.TestCase):
    def test_isinstance(self):
        s = slice(1, 5)
        self.assertIsInstance(s, slice)

    def test_attrs(self):
        s = slice(1, 5, 2)
        self.assertEqual(s.start, 1)
        self.assertEqual(s.stop, 5)
        self.assertEqual(s.step, 2)


class GetitemSliceTest(unittest.TestCase):
    def test_dispatch(self):
        events = []
        class S:
            def __getitem__(self, k):
                events.append(k)
                return k
        s = S()
        s[1:5:2]
        self.assertIsInstance(events[0], slice)
        self.assertEqual(events[0].start, 1)


class CustomReversedTest(unittest.TestCase):
    def test_reversed_dunder(self):
        class R:
            def __reversed__(self):
                yield 3; yield 2; yield 1
        self.assertEqual(list(reversed(R())), [3, 2, 1])


class OperatorOverloadTest(unittest.TestCase):
    def test_arith(self):
        class Vec:
            def __init__(self, x, y): self.x = x; self.y = y
            def __add__(self, o): return Vec(self.x + o.x, self.y + o.y)
            def __mul__(self, k): return Vec(self.x * k, self.y * k)
            def __rmul__(self, k): return Vec(self.x * k, self.y * k)
            def __neg__(self): return Vec(-self.x, -self.y)
            def __eq__(self, o): return self.x == o.x and self.y == o.y
        a = Vec(1, 2); b = Vec(3, 4)
        self.assertEqual(a + b, Vec(4, 6))
        self.assertEqual(a * 3, Vec(3, 6))
        self.assertEqual(3 * a, Vec(3, 6))
        self.assertEqual(-a, Vec(-1, -2))


class NumKeyEqTest(unittest.TestCase):
    def test_int_float_collide(self):
        d = {1: "a", 1.0: "b"}
        self.assertEqual(len(d), 1)
        self.assertEqual(d[1], "b")

    def test_bool_int_collide(self):
        d = {True: "a"}
        d[1] = "b"
        self.assertEqual(d, {True: "b"})


unittest.main(globals())
