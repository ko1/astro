import unittest


class IterSentinelTest(unittest.TestCase):
    def test_basic(self):
        counter = [0]
        def f():
            counter[0] += 1
            return counter[0]
        self.assertEqual(list(iter(f, 5)), [1, 2, 3, 4])

    def test_immediate_stop(self):
        def f():
            return 0
        self.assertEqual(list(iter(f, 0)), [])


class ParamMarkerTest(unittest.TestCase):
    def test_kwonly(self):
        def f(a, *, b):
            return a + b
        self.assertEqual(f(1, b=2), 3)

    def test_kwonly_with_default(self):
        def f(a, *, b=10):
            return a + b
        self.assertEqual(f(1), 11)
        self.assertEqual(f(1, b=2), 3)

    def test_posonly(self):
        def f(a, b, /, c):
            return a + b + c
        self.assertEqual(f(1, 2, 3), 6)
        self.assertEqual(f(1, 2, c=3), 6)

    def test_combined(self):
        def f(a, b, /, c, *, d):
            return a + b + c + d
        self.assertEqual(f(1, 2, 3, d=4), 10)


class SpreadInLiteralTest(unittest.TestCase):
    def test_list_spread(self):
        a = [1, 2]
        b = [3, 4]
        self.assertEqual([*a, *b], [1, 2, 3, 4])
        self.assertEqual([*a, 99, *b], [1, 2, 99, 3, 4])
        self.assertEqual([0, *a, 99], [0, 1, 2, 99])

    def test_tuple_spread(self):
        a = (1, 2)
        self.assertEqual((*a, 3, 4), (1, 2, 3, 4))
        self.assertEqual((*"ab",), ("a", "b"))

    def test_set_spread(self):
        s1 = {1, 2}
        self.assertEqual({*s1, 3}, {1, 2, 3})

    def test_dict_spread(self):
        a = {"x": 1}
        b = {"y": 2}
        self.assertEqual({**a, **b}, {"x": 1, "y": 2})
        self.assertEqual({**a, "z": 3}, {"x": 1, "z": 3})
        # Later keys override earlier.
        self.assertEqual({**a, "x": 99}, {"x": 99})


class BoundMethodMetaTest(unittest.TestCase):
    def test_doc(self):
        class C:
            def m(self):
                """method doc"""
                return 1
        c = C()
        self.assertEqual(c.m.__doc__, "method doc")

    def test_self_func(self):
        class C:
            def m(self):
                return 1
        c = C()
        self.assertIs(c.m.__self__, c)


class FormatSpecTest(unittest.TestCase):
    def test_alt_form(self):
        self.assertEqual(f"{255:#x}", "0xff")
        self.assertEqual(f"{255:#X}", "0XFF")
        self.assertEqual(f"{8:#o}", "0o10")
        self.assertEqual(f"{10:#b}", "0b1010")

    def test_thousands(self):
        self.assertEqual(f"{1000000:,}", "1,000,000")
        self.assertEqual(f"{-1234567:,}", "-1,234,567")
        self.assertEqual(f"{42:,}", "42")

    def test_percent(self):
        self.assertEqual(f"{0.25:.0%}", "25%")
        self.assertEqual(f"{0.5:.2%}", "50.00%")

    def test_sign(self):
        self.assertEqual(f"{42:+d}", "+42")
        self.assertEqual(f"{-42:+d}", "-42")


unittest.main(globals())
