# Misc cpython-style tests: scoping, slicing, control flow, comprehensions.

import unittest


class ScopeTest(unittest.TestCase):
    GLOBAL = 100

    def test_global_in_method(self):
        # Methods read globals via the function's captured globals.
        self.assertEqual(ScopeTest.GLOBAL, 100)

    def test_lookup_chain(self):
        x = 5
        def inner():
            return x       # closure
        self.assertEqual(inner(), 5)


class SliceTest(unittest.TestCase):
    def test_list_slice(self):
        a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        self.assertEqual(a[2:5], [2, 3, 4])
        self.assertEqual(a[:3], [0, 1, 2])
        self.assertEqual(a[7:], [7, 8, 9])
        self.assertEqual(a[-3:], [7, 8, 9])
        self.assertEqual(a[:-3], [0, 1, 2, 3, 4, 5, 6])
        self.assertEqual(a[::2], [0, 2, 4, 6, 8])
        self.assertEqual(a[::-1], list(reversed(a)))
        self.assertEqual(a[1:8:3], [1, 4, 7])
        self.assertEqual(a[100:], [])
        self.assertEqual(a[:0], [])

    def test_str_slice(self):
        s = "abcdefghij"
        self.assertEqual(s[2:5], "cde")
        self.assertEqual(s[::2], "acegi")
        self.assertEqual(s[::-1], "jihgfedcba")

    def test_tuple_slice(self):
        t = (1, 2, 3, 4, 5)
        self.assertEqual(t[1:4], (2, 3, 4))
        self.assertEqual(t[::-1], (5, 4, 3, 2, 1))

    def test_slice_assign(self):
        a = [0, 1, 2, 3, 4]
        a[1:3] = [10, 20, 30]
        self.assertEqual(a, [0, 10, 20, 30, 3, 4])
        a[2:5] = []
        self.assertEqual(a, [0, 10, 4])


class ControlTest(unittest.TestCase):
    def test_if_chain(self):
        def classify(x):
            if x < 0: return "neg"
            elif x == 0: return "zero"
            elif x < 10: return "small"
            else: return "big"
        self.assertEqual(classify(-1), "neg")
        self.assertEqual(classify(0), "zero")
        self.assertEqual(classify(5), "small")
        self.assertEqual(classify(100), "big")

    def test_while_else(self):
        ran = []
        i = 0
        while i < 3:
            ran.append(i)
            i += 1
        else:
            ran.append("else")
        self.assertEqual(ran, [0, 1, 2, "else"])

    def test_while_break(self):
        ran = []
        i = 0
        while i < 5:
            if i == 3: break
            ran.append(i)
            i += 1
        else:
            ran.append("else")     # NOT executed when break
        self.assertEqual(ran, [0, 1, 2])

    def test_for_else(self):
        ran = []
        for x in range(3):
            ran.append(x)
        else:
            ran.append("else")
        self.assertEqual(ran, [0, 1, 2, "else"])

    def test_for_break(self):
        ran = []
        for x in range(5):
            if x == 3: break
            ran.append(x)
        else:
            ran.append("else")
        self.assertEqual(ran, [0, 1, 2])

    def test_continue(self):
        even = []
        for x in range(10):
            if x % 2 != 0: continue
            even.append(x)
        self.assertEqual(even, [0, 2, 4, 6, 8])

    def test_chained_compare(self):
        self.assertTrue(1 < 2 < 3)
        self.assertFalse(1 < 5 < 3)
        self.assertTrue(1 == 1 == 1)
        self.assertFalse(1 == 1 == 2)
        self.assertTrue(1 < 2 < 3 < 4 < 5)

    def test_short_circuit(self):
        def loud():
            self.fail("should not be called")
        # `False and X` doesn't call X.
        self.assertFalse(False and loud())
        # `True or X` doesn't call X.
        self.assertTrue(True or loud())

    def test_walrus(self):
        if (n := 5) > 0:
            self.assertEqual(n, 5)

    def test_conditional_expr(self):
        x = "yes" if True else "no"
        self.assertEqual(x, "yes")
        y = [1 if i > 0 else -1 for i in [-2, 0, 3]]
        self.assertEqual(y, [-1, -1, 1])


class CompTest(unittest.TestCase):
    def test_listcomp(self):
        self.assertEqual([x*2 for x in range(5)], [0, 2, 4, 6, 8])
        self.assertEqual([x for x in range(10) if x % 2 == 0], [0, 2, 4, 6, 8])

    def test_setcomp(self):
        s = {x % 5 for x in range(20)}
        self.assertEqual(sorted(s), [0, 1, 2, 3, 4])

    def test_dictcomp(self):
        d = {x: x*x for x in range(5)}
        self.assertEqual(d, {0: 0, 1: 1, 2: 4, 3: 9, 4: 16})

    def test_genexp(self):
        g = (x*x for x in range(5))
        self.assertEqual(list(g), [0, 1, 4, 9, 16])

    def test_nested_comp(self):
        m = [[i*j for j in range(3)] for i in range(3)]
        self.assertEqual(m, [[0, 0, 0], [0, 1, 2], [0, 2, 4]])

    def test_comp_with_conditions(self):
        out = [x for x in range(20) if x % 2 == 0 if x > 5]
        self.assertEqual(out, [6, 8, 10, 12, 14, 16, 18])


unittest.main(globals())
