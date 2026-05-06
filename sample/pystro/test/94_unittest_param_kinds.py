import unittest


class KwOnlyTest(unittest.TestCase):
    def test_basic(self):
        def f(a, *, b, c=10): return (a, b, c)
        self.assertEqual(f(1, b=2), (1, 2, 10))
        self.assertEqual(f(1, b=2, c=3), (1, 2, 3))

    def test_positional_rejected(self):
        def f(a, *, b): return (a, b)
        with self.assertRaises(TypeError):
            f(1, 2)

    def test_missing_required(self):
        def f(a, *, b): return (a, b)
        with self.assertRaises(TypeError):
            f(1)


class PosOnlyTest(unittest.TestCase):
    def test_basic(self):
        def f(a, b, /, c): return (a, b, c)
        self.assertEqual(f(1, 2, 3), (1, 2, 3))
        self.assertEqual(f(1, 2, c=3), (1, 2, 3))

    def test_kwarg_rejected(self):
        def f(a, b, /): return (a, b)
        with self.assertRaises(TypeError):
            f(1, b=2)

    def test_kwargs_collects_pos_only_name(self):
        # When **kwargs present, a kwarg with pos-only name lands in **kwargs.
        def f(a, /, **kw): return (a, kw)
        result = f(1, a=2)
        self.assertEqual(result[0], 1)
        self.assertEqual(result[1], {"a": 2})


class SlotsTest(unittest.TestCase):
    def test_allowed(self):
        class P:
            __slots__ = ("x", "y")
        p = P()
        p.x = 1
        p.y = 2
        self.assertEqual(p.x, 1)
        self.assertEqual(p.y, 2)

    def test_rejected(self):
        class P:
            __slots__ = ("x",)
        p = P()
        with self.assertRaises(AttributeError):
            p.z = 99

    def test_subclass_inherits(self):
        class A:
            __slots__ = ("x",)
        class B(A):
            __slots__ = ("y",)
        b = B()
        b.x = 1
        b.y = 2
        with self.assertRaises(AttributeError):
            b.z = 3

    def test_subclass_no_slots_allows(self):
        # If any class in MRO has no __slots__, instance gets __dict__.
        class A:
            __slots__ = ("x",)
        class B(A):  # no __slots__ on B
            pass
        b = B()
        b.x = 1
        b.z = 2  # allowed because B has no __slots__
        self.assertEqual(b.x, 1)
        self.assertEqual(b.z, 2)


unittest.main(globals())
