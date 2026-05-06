import unittest


class CompScopeTest(unittest.TestCase):
    def test_listcomp_doesnt_leak(self):
        # Function-level: comp loop var should not leak to outer.
        def f():
            [i for i in range(3)]
            try:
                _ = i
                return "leaked"
            except NameError:
                return "ok"
        self.assertEqual(f(), "ok")

    def test_setcomp_doesnt_leak(self):
        def f():
            {x for x in range(3)}
            try:
                _ = x
                return "leaked"
            except NameError:
                return "ok"
        self.assertEqual(f(), "ok")

    def test_dictcomp_doesnt_leak(self):
        def f():
            {k: v for k, v in [(1, 2)]}
            try:
                _ = k
                return "leaked-k"
            except NameError: pass
            try:
                _ = v
                return "leaked-v"
            except NameError: pass
            return "ok"
        self.assertEqual(f(), "ok")

    def test_comp_doesnt_clobber_outer(self):
        def f():
            i = "outer"
            [i for i in range(3)]
            return i
        self.assertEqual(f(), "outer")

    def test_lambda_default_capture_in_comp(self):
        # Classic CPython idiom: i=i to capture per-iteration.
        def make_adders():
            return [(lambda x, i=i: x + i) for i in range(3)]
        adders = make_adders()
        self.assertEqual([f(0) for f in adders], [0, 1, 2])

    def test_nested_comp(self):
        def f():
            matrix = [[1, 2], [3, 4]]
            return [[v + 1 for v in row] for row in matrix]
        self.assertEqual(f(), [[2, 3], [4, 5]])

    def test_walrus_in_comp_still_leaks(self):
        # Walrus has explicit "leak to enclosing scope" semantics.
        def f():
            result = [y for x in [1, 2, 3] if (y := x * 2) > 0]
            return result, y  # y should leak
        result, y = f()
        self.assertEqual(result, [2, 4, 6])
        self.assertEqual(y, 6)


unittest.main(globals())
