import unittest


class YieldFromTest(unittest.TestCase):
    def test_basic(self):
        def inner():
            yield 1
            yield 2
        def outer():
            yield 0
            yield from inner()
            yield 3
        self.assertEqual(list(outer()), [0, 1, 2, 3])

    def test_capture_return_value(self):
        def inner():
            yield 1
            yield 2
            return 99
        def outer():
            val = yield from inner()
            yield val
        self.assertEqual(list(outer()), [1, 2, 99])

    def test_yield_from_list(self):
        def g():
            yield from [10, 20, 30]
        self.assertEqual(list(g()), [10, 20, 30])

    def test_nested_yield_from(self):
        def a():
            yield 1
            return "a-done"
        def b():
            v = yield from a()
            yield 2
            return v
        def c():
            v = yield from b()
            yield v
        self.assertEqual(list(c()), [1, 2, "a-done"])


class GeneratorCloseTest(unittest.TestCase):
    def test_close_runs_finally(self):
        log = []
        def g():
            try:
                yield 1
                yield 2
            except GeneratorExit:
                log.append("exit")
        gen = g()
        next(gen)
        gen.close()
        self.assertEqual(log, ["exit"])

    def test_close_unstarted(self):
        def g():
            yield 1
        gen = g()
        gen.close()  # should be a no-op


class StopIterationValueTest(unittest.TestCase):
    def test_value(self):
        def g():
            yield 1
            return "done"
        gen = g()
        try:
            while True:
                next(gen)
        except StopIteration as e:
            self.assertEqual(e.value, "done")


unittest.main(globals())
