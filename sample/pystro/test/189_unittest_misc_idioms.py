"""Smoke test for common Python idioms — protects against regressions."""
import unittest


class GeneratorEdgeTest(unittest.TestCase):
    def test_send_unstarted(self):
        def g():
            yield 1
        with self.assertRaises(TypeError):
            g().send(42)

    def test_close_runs_finally(self):
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

    def test_yield_from_returns(self):
        def inner():
            yield 1
            return "ret"
        def outer():
            r = yield from inner()
            yield ("got:" + r)
        self.assertEqual(list(outer()), [1, "got:ret"])


class TryExceptElseTest(unittest.TestCase):
    def test_else_runs_only_no_exception(self):
        events = []
        def f():
            try: events.append("try")
            except: events.append("except")
            else: events.append("else")
        f()
        self.assertEqual(events, ["try", "else"])

    def test_else_skipped_when_exception(self):
        events = []
        def f():
            try: raise ValueError; events.append("after")
            except: events.append("except")
            else: events.append("else")
        f()
        self.assertEqual(events, ["except"])


class CustomFormatTest(unittest.TestCase):
    def test_dispatch(self):
        class Money:
            def __init__(self, n): self.n = n
            def __format__(self, spec):
                if spec == "":
                    return f"${self.n}"
                return format(self.n, spec)
        self.assertEqual(f"{Money(100)}", "$100")
        self.assertEqual(f"{Money(100):.2f}", "100.00")


class IterDunderTest(unittest.TestCase):
    def test_iter_yields_generator(self):
        class GW:
            def __iter__(self):
                yield "a"; yield "b"
        self.assertEqual(list(GW()), ["a", "b"])

    def test_iter_returns_self(self):
        class It:
            def __init__(self, n): self.n = n; self.i = 0
            def __iter__(self): return self
            def __next__(self):
                if self.i >= self.n: raise StopIteration
                self.i += 1
                return self.i
        self.assertEqual(list(It(3)), [1, 2, 3])


unittest.main(globals())
