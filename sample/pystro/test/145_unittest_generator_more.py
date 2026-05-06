import unittest


class GeneratorTest(unittest.TestCase):
    def test_basic(self):
        def g():
            yield 1
            yield 2
            yield 3
        self.assertEqual(list(g()), [1, 2, 3])

    def test_return_value(self):
        def g():
            yield 1
            return "done"
        gi = g()
        self.assertEqual(next(gi), 1)
        try:
            next(gi)
            self.fail("no StopIteration")
        except StopIteration as e:
            self.assertEqual(e.value, "done")

    def test_yield_from(self):
        def inner():
            yield 1; yield 2
            return "ID"
        def outer():
            r = yield from inner()
            yield ("got " + r)
        self.assertEqual(list(outer()), [1, 2, "got ID"])

    def test_send(self):
        def echo():
            while True:
                x = yield
                if x is None: break
                yield x * 2
        gi = echo()
        next(gi)
        self.assertEqual(gi.send(5), 10)
        next(gi)
        self.assertEqual(gi.send(7), 14)

    def test_throw_two_arg(self):
        def g():
            try:
                yield 1
            except ValueError:
                yield "caught"
        gi = g()
        next(gi)
        self.assertEqual(gi.throw(ValueError("bad")), "caught")

    def test_throw_three_arg(self):
        # CPython 3.11 still allows throw(type, value, [tb]).
        def g():
            try:
                yield 1
            except RuntimeError as e:
                yield str(e)
        gi = g()
        next(gi)
        self.assertEqual(gi.throw(RuntimeError, "oops"), "oops")

    def test_close(self):
        events = []
        def g():
            try:
                yield 1
                yield 2
            finally:
                events.append("cleanup")
        gi = g()
        next(gi)
        gi.close()
        self.assertEqual(events, ["cleanup"])

    def test_iter_self(self):
        def g():
            yield 1
        gi = g()
        self.assertIs(iter(gi), gi)


class CustomIteratorTest(unittest.TestCase):
    def test_counter(self):
        class Counter:
            def __init__(self, n): self.n = n; self.i = 0
            def __iter__(self): return self
            def __next__(self):
                if self.i >= self.n: raise StopIteration
                self.i += 1
                return self.i
        self.assertEqual(list(Counter(3)), [1, 2, 3])

    def test_iter_sentinel(self):
        class Source:
            def __init__(self): self.it = iter([1, 2, 3, 0, 4])
            def __call__(self): return next(self.it)
        self.assertEqual(list(iter(Source(), 0)), [1, 2, 3])

    def test_next_default(self):
        it = iter([])
        self.assertEqual(next(it, "X"), "X")


unittest.main(globals())
