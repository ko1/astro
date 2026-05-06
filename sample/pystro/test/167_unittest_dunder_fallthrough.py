import unittest


class NotImplementedFallbackTest(unittest.TestCase):
    def test_add_falls_through(self):
        class T1:
            def __add__(self, o):
                if isinstance(o, T2): return NotImplemented
                return "T1+"
        class T2:
            def __radd__(self, o):
                return "T2-radd"
        self.assertEqual(T1() + T2(), "T2-radd")

    def test_eq_falls_through(self):
        # If __eq__ returns NotImplemented, the comparison falls through
        # to identity / reflected eq.
        class A:
            def __eq__(self, o): return NotImplemented
        class B:
            def __eq__(self, o): return True
        self.assertTrue(A() == B())


class TruncProtocolTest(unittest.TestCase):
    def test_trunc_dunder(self):
        import math
        class T:
            def __trunc__(self): return "trunc!"
        self.assertEqual(math.trunc(T()), "trunc!")

    def test_floor_dunder(self):
        import math
        class T:
            def __floor__(self): return "floor!"
        self.assertEqual(math.floor(T()), "floor!")

    def test_ceil_dunder(self):
        import math
        class T:
            def __ceil__(self): return "ceil!"
        self.assertEqual(math.ceil(T()), "ceil!")


class RoundTest(unittest.TestCase):
    def test_round_dunder(self):
        class R:
            def __round__(self, ndigits=0): return ("r", ndigits)
        self.assertEqual(round(R()), ("r", 0))
        self.assertEqual(round(R(), 3), ("r", 3))


unittest.main(globals())
