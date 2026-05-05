import unittest


class BoolDunderTest(unittest.TestCase):
    def test_bool_dunder(self):
        class Falsy:
            def __bool__(self):
                return False
        f = Falsy()
        self.assertEqual(bool(f), False)
        self.assertEqual("yes" if f else "no", "no")

    def test_len_dunder_for_bool(self):
        class Empty:
            def __len__(self):
                return 0
        class Filled:
            def __len__(self):
                return 3
        self.assertFalse(bool(Empty()))
        self.assertTrue(bool(Filled()))


class IsInstanceTest(unittest.TestCase):
    def test_bool_is_int(self):
        self.assertTrue(isinstance(True, int))
        self.assertTrue(isinstance(False, int))
        self.assertTrue(isinstance(True, bool))
        self.assertFalse(isinstance(5, bool))


class FuncDocstringTest(unittest.TestCase):
    def test_doc(self):
        def f():
            """function doc"""
            return 1
        self.assertEqual(f.__doc__, "function doc")

    def test_no_doc(self):
        def f():
            return 1
        self.assertIsNone(f.__doc__)


class NegAbsTest(unittest.TestCase):
    def test_neg(self):
        class N:
            def __init__(self, v): self.v = v
            def __neg__(self):
                return N(-self.v)
            def __eq__(self, o): return self.v == o.v
        self.assertEqual(-N(5), N(-5))

    def test_abs(self):
        class A:
            def __init__(self, v): self.v = v
            def __abs__(self):
                return A(self.v if self.v >= 0 else -self.v)
            def __eq__(self, o): return self.v == o.v
        self.assertEqual(abs(A(-3)), A(3))


unittest.main(globals())
