"""S-18 (lazy genexp), S-23 (PEP 654 except*), S-24 (PEP 695 type alias)."""
import unittest


class LazyGenexpTest(unittest.TestCase):
    def test_genexp_is_generator(self):
        g = (x for x in range(3))
        self.assertEqual(type(g).__name__, "generator")

    def test_short_circuit_all(self):
        calls = []
        def check(x):
            calls.append(x); return x
        all(check(x) for x in [True, False, True])
        self.assertEqual(calls, [True, False])

    def test_short_circuit_any(self):
        calls = []
        def check(x):
            calls.append(x); return x
        any(check(x) for x in [False, True, False])
        self.assertEqual(calls, [False, True])

    def test_infinite_source(self):
        import itertools
        g = (x for x in itertools.count(0))
        self.assertEqual(next(g), 0)
        self.assertEqual(next(g), 1)
        self.assertEqual(next(g), 2)

    def test_closure_capture(self):
        def make(n):
            return (x * n for x in range(3))
        self.assertEqual(list(make(10)), [0, 10, 20])
        # Each call creates a fresh genexp with its own n.
        g1 = make(10)
        g2 = make(20)
        self.assertEqual(list(g1), [0, 10, 20])
        self.assertEqual(list(g2), [0, 20, 40])

    def test_outer_iter_evaluated_once(self):
        calls = []
        def src():
            calls.append("called"); return [1, 2, 3]
        g = (x for x in src())
        self.assertEqual(calls, ["called"])
        list(g)
        self.assertEqual(calls, ["called"])

    def test_inner_iter_evaluated_each_time(self):
        calls = []
        def inner():
            calls.append("inner"); return [10, 20]
        g = ((x, y) for x in [1, 2, 3] for y in inner())
        list(g)
        self.assertEqual(len(calls), 3)

    def test_no_leak_outside(self):
        list(j for j in range(5))
        with self.assertRaises(NameError):
            print(j)  # noqa

    def test_with_filter(self):
        self.assertEqual(list(x for x in range(10) if x % 2 == 0),
                         [0, 2, 4, 6, 8])

    def test_nested_for(self):
        self.assertEqual(list(x*y for x in range(3) for y in range(3)),
                         [0, 0, 0, 0, 1, 2, 0, 2, 4])


class ExceptionGroupTest(unittest.TestCase):
    def test_basic_construct(self):
        eg = ExceptionGroup("multi", [ValueError("a"), TypeError("b")])
        self.assertEqual(eg.message, "multi")
        self.assertEqual(len(eg.exceptions), 2)
        self.assertIsInstance(eg, BaseExceptionGroup)
        self.assertIsInstance(eg, BaseException)

    def test_except_star_single_match(self):
        try:
            raise ExceptionGroup("multi", [ValueError("a")])
        except* ValueError as eg:
            self.assertEqual(len(eg.exceptions), 1)

    def test_except_star_multi_handlers(self):
        ve_caught = te_caught = False
        try:
            raise ExceptionGroup("multi", [ValueError("a"), TypeError("b")])
        except* ValueError:
            ve_caught = True
        except* TypeError:
            te_caught = True
        self.assertTrue(ve_caught)
        self.assertTrue(te_caught)

    def test_except_star_unmatched_reraises(self):
        with self.assertRaises(ExceptionGroup):
            try:
                raise ExceptionGroup("multi", [ValueError("a"), RuntimeError("b")])
            except* ValueError:
                pass

    def test_bare_exception_falls_through(self):
        # except* doesn't catch bare exceptions in CPython.
        with self.assertRaises(ValueError):
            try:
                raise ValueError("bare")
            except* ValueError:
                pass


class TypeAliasTest(unittest.TestCase):
    def test_simple_alias(self):
        type X = int
        self.assertIs(X, int)

    def test_generic_alias(self):
        type Vec = list[int]
        self.assertIs(Vec, list)


unittest.main(globals())
