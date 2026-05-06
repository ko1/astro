import unittest


class YieldFromCloseTest(unittest.TestCase):
    def test_inner_finally_runs(self):
        events = []
        def inner():
            try:
                yield 1; yield 2
            finally:
                events.append("inner")

        def outer():
            try:
                yield from inner()
            finally:
                events.append("outer")

        g = outer()
        next(g)
        g.close()
        self.assertEqual(events, ["inner", "outer"])

    def test_inner_only(self):
        events = []
        def inner():
            try:
                yield 1
            finally:
                events.append("inner")

        def outer():
            yield from inner()

        g = outer()
        next(g)
        g.close()
        self.assertEqual(events, ["inner"])

    def test_throw_into_outer_propagates(self):
        events = []
        def inner():
            try:
                yield 1
            except ValueError:
                events.append("inner-caught")
                yield 99

        def outer():
            yield from inner()

        g = outer()
        next(g)
        result = g.throw(ValueError)
        # inner caught and yielded 99
        self.assertEqual(events, ["inner-caught"])

    def test_chain_three_levels(self):
        events = []
        def innermost():
            try:
                yield 1
            finally:
                events.append("innermost")

        def middle():
            try:
                yield from innermost()
            finally:
                events.append("middle")

        def outermost():
            try:
                yield from middle()
            finally:
                events.append("outermost")

        g = outermost()
        next(g)
        g.close()
        self.assertEqual(events, ["innermost", "middle", "outermost"])


unittest.main(globals())
