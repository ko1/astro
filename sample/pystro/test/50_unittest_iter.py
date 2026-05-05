# Adapted from CPython test_iter.py / test_generators.py.

import unittest


class IterTest(unittest.TestCase):
    def test_list_iter(self):
        out = []
        for x in [1, 2, 3]:
            out.append(x)
        self.assertEqual(out, [1, 2, 3])

    def test_str_iter(self):
        self.assertEqual(list("abc"), ["a", "b", "c"])

    def test_dict_iter(self):
        d = {"a": 1, "b": 2, "c": 3}
        self.assertEqual(list(d), ["a", "b", "c"])

    def test_range_iter(self):
        self.assertEqual(list(range(5)), [0, 1, 2, 3, 4])
        self.assertEqual(list(range(2, 5)), [2, 3, 4])
        self.assertEqual(list(range(0, 10, 2)), [0, 2, 4, 6, 8])
        self.assertEqual(list(range(10, 0, -2)), [10, 8, 6, 4, 2])

    def test_zip(self):
        self.assertEqual(list(zip([1, 2, 3], "abc")), [(1, "a"), (2, "b"), (3, "c")])
        self.assertEqual(list(zip([1, 2, 3], [10, 20])), [(1, 10), (2, 20)])

    def test_enumerate(self):
        self.assertEqual(list(enumerate(["a", "b", "c"])), [(0, "a"), (1, "b"), (2, "c")])
        self.assertEqual(list(enumerate(["a", "b"], start=10)), [(10, "a"), (11, "b")])

    def test_map(self):
        self.assertEqual(list(map(lambda x: x*2, [1, 2, 3])), [2, 4, 6])

    def test_filter(self):
        self.assertEqual(list(filter(lambda x: x > 2, [1, 2, 3, 4])), [3, 4])

    def test_iter_next(self):
        it = iter([1, 2, 3])
        self.assertEqual(next(it), 1)
        self.assertEqual(next(it), 2)
        self.assertEqual(next(it), 3)
        try:
            next(it)
            self.fail("expected StopIteration")
        except StopIteration:
            pass

    def test_iter_default(self):
        it = iter([1])
        self.assertEqual(next(it), 1)
        self.assertEqual(next(it, "DEFAULT"), "DEFAULT")

    def test_custom_iter(self):
        class CountUp:
            def __init__(self, n):
                self.n = n
                self.i = 0
            def __iter__(self):
                return self
            def __next__(self):
                if self.i >= self.n:
                    raise StopIteration
                v = self.i
                self.i += 1
                return v

        self.assertEqual(list(CountUp(4)), [0, 1, 2, 3])
        # Re-iter: each instance starts from beginning (same as Python).
        c = CountUp(3)
        self.assertEqual(list(c), [0, 1, 2])

    def test_generator_basic(self):
        def gen(n):
            for i in range(n):
                yield i * 2
        self.assertEqual(list(gen(4)), [0, 2, 4, 6])

    def test_generator_state(self):
        def fib():
            a, b = 0, 1
            while True:
                yield a
                a, b = b, a + b
        g = fib()
        out = []
        for _ in range(10):
            out.append(next(g))
        self.assertEqual(out, [0, 1, 1, 2, 3, 5, 8, 13, 21, 34])

    def test_generator_return(self):
        def early(n):
            for i in range(n):
                if i == 3:
                    return
                yield i
        self.assertEqual(list(early(10)), [0, 1, 2])

    def test_nested_generators(self):
        def inner(n):
            for i in range(n):
                yield i
        def outer(n):
            for v in inner(n):
                yield v * 10
        self.assertEqual(list(outer(4)), [0, 10, 20, 30])

    def test_genexp(self):
        self.assertEqual(sum(x*x for x in range(5)), 30)
        self.assertEqual(list(x for x in range(3)), [0, 1, 2])
        self.assertEqual(", ".join(str(x) for x in [1, 2, 3]), "1, 2, 3")

    def test_reversed(self):
        self.assertEqual(list(reversed([1, 2, 3])), [3, 2, 1])
        self.assertEqual(list(reversed("abc")), ["c", "b", "a"])

    def test_sum_min_max(self):
        self.assertEqual(sum([1, 2, 3]), 6)
        self.assertEqual(sum(range(10)), 45)
        self.assertEqual(min([3, 1, 2]), 1)
        self.assertEqual(max([3, 1, 2]), 3)
        self.assertEqual(min([], default=99), 99)

    def test_all_any(self):
        self.assertTrue(all([1, 2, 3]))
        self.assertFalse(all([1, 0, 2]))
        self.assertTrue(any([0, 0, 1]))
        self.assertFalse(any([]))
        self.assertTrue(all([]))


unittest.main(globals())
