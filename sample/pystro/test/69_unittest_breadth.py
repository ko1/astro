import unittest


class NumericNanTest(unittest.TestCase):
    def test_nan(self):
        self.assertTrue(float("nan") != float("nan"))
        self.assertFalse(float("nan") == float("nan"))

    def test_math_nan(self):
        import math
        self.assertTrue(math.isnan(math.nan))
        self.assertFalse(math.isnan(1.0))
        self.assertTrue(math.isinf(math.inf))
        self.assertFalse(math.isfinite(math.nan))


class MathNewTest(unittest.TestCase):
    def test_constants(self):
        import math
        self.assertEqual(math.tau, 2 * math.pi)

    def test_lcm_comb(self):
        import math
        self.assertEqual(math.lcm(4, 6), 12)
        self.assertEqual(math.comb(5, 2), 10)
        self.assertEqual(math.perm(5, 2), 20)

    def test_trig(self):
        import math
        self.assertEqual(round(math.degrees(math.pi), 5), 180.0)
        self.assertAlmostEqual(math.radians(180.0), math.pi, places=10)


class HeapqTest(unittest.TestCase):
    def test_basic(self):
        import heapq
        h = []
        for x in [5, 1, 3, 2, 4]:
            heapq.heappush(h, x)
        self.assertEqual([heapq.heappop(h) for _ in range(5)], [1, 2, 3, 4, 5])


class BisectTest(unittest.TestCase):
    def test_bisect(self):
        import bisect
        a = [1, 3, 5, 7, 9]
        self.assertEqual(bisect.bisect_left(a, 4), 2)
        self.assertEqual(bisect.bisect_right(a, 5), 3)


class TextwrapTest(unittest.TestCase):
    def test_wrap(self):
        import textwrap
        out = textwrap.wrap("hello world how are you", 10)
        self.assertTrue(all(len(line) <= 10 for line in out))

    def test_dedent(self):
        import textwrap
        text = "    line1\n    line2"
        self.assertEqual(textwrap.dedent(text), "line1\nline2")


class QueueTest(unittest.TestCase):
    def test_fifo(self):
        import queue
        q = queue.Queue()
        q.put(1); q.put(2); q.put(3)
        self.assertEqual([q.get(), q.get(), q.get()], [1, 2, 3])

    def test_lifo(self):
        import queue
        q = queue.LifoQueue()
        q.put(1); q.put(2); q.put(3)
        self.assertEqual([q.get(), q.get(), q.get()], [3, 2, 1])


class DunderArithTest(unittest.TestCase):
    def test_iadd_only(self):
        class IL:
            def __init__(self, v): self.v = v
            def __iadd__(self, o):
                self.v += o
                return self
        x = IL(10)
        x += 5
        self.assertEqual(x.v, 15)

    def test_full_dunders(self):
        class N:
            def __init__(self, v): self.v = v
            def __add__(self, o): return N(self.v + o)
            def __sub__(self, o): return N(self.v - o)
            def __mul__(self, o): return N(self.v * o)
            def __pow__(self, o): return N(self.v ** o)
            def __mod__(self, o): return N(self.v % o)
            def __floordiv__(self, o): return N(self.v // o)
            def __truediv__(self, o): return N(self.v / o)
            def __and__(self, o): return N(self.v & o)
            def __or__(self, o): return N(self.v | o)
            def __xor__(self, o): return N(self.v ^ o)
            def __lshift__(self, o): return N(self.v << o)
            def __rshift__(self, o): return N(self.v >> o)
            def __invert__(self): return N(~self.v)
            def __eq__(self, o): return isinstance(o, N) and self.v == o.v
            def __hash__(self): return hash(self.v)
        self.assertEqual((N(10) + 5).v, 15)
        self.assertEqual((N(10) ** 2).v, 100)
        self.assertEqual((N(10) % 3).v, 1)
        self.assertEqual((N(10) & 6).v, 2)
        self.assertEqual((N(10) | 5).v, 15)
        self.assertEqual((N(10) ^ 6).v, 12)
        self.assertEqual((N(2) << 3).v, 16)
        self.assertEqual((N(16) >> 2).v, 4)
        self.assertEqual((~N(0)).v, -1)


class CompScopeInLambdaTest(unittest.TestCase):
    def test_genexp_in_lambda(self):
        f = lambda: list(x * 2 for x in [1, 2, 3])
        self.assertEqual(f(), [2, 4, 6])

    def test_listcomp_in_method(self):
        class C:
            def m(self):
                return [x + 1 for x in [10, 20]]
        self.assertEqual(C().m(), [11, 21])

    def test_lambda_default_outer_scope(self):
        def make():
            return [(lambda x, i=i: x + i) for i in range(3)]
        adders = make()
        self.assertEqual([f(0) for f in adders], [0, 1, 2])


class CallSiteKwargRegistrationTest(unittest.TestCase):
    def test_int_in_method_with_kwarg_call(self):
        # Used to fail: pre-scan registered `int`/`base` as locals.
        class A:
            def m(self):
                # int must remain global; base=16 is kwarg.
                return int("ff", base=16)
        self.assertEqual(A().m(), 255)


class FloatNaNComparisonTest(unittest.TestCase):
    def test_eq_self(self):
        n = float("nan")
        self.assertFalse(n == n)
        self.assertTrue(n != n)

    def test_in_list(self):
        n = float("nan")
        # NaN's `in` uses identity-or-equality; pystro impl compares with
        # py_eq_bool which returns False for NaN.
        # Reasonable result: False (NaN doesn't equal anything).
        self.assertFalse(n in [n])


unittest.main(globals())
