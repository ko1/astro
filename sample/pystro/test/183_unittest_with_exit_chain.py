import unittest


class WithExitChainTest(unittest.TestCase):
    def test_exit_raises_new_exc_chained(self):
        # When __exit__ raises a new exception while the body's exception is
        # active, the new exception's __context__ should be the original.
        class ER:
            def __enter__(self): return self
            def __exit__(self, t, v, tb):
                raise RuntimeError("from exit")
        with self.assertRaises(RuntimeError) as cm:
            with ER():
                raise ValueError("body")
        self.assertIsInstance(cm.exception.__context__, ValueError)

    def test_enter_raise_skips_exit(self):
        # When __enter__ raises, __exit__ is NOT called.
        called = []
        class BE:
            def __enter__(self): raise ValueError("bad")
            def __exit__(self, *a):
                called.append("exit")
                return False
        with self.assertRaises(ValueError):
            with BE():
                pass
        self.assertEqual(called, [])

    def test_multi_with_lifo(self):
        events = []
        class T:
            def __init__(self, n): self.n = n
            def __enter__(self):
                events.append(f"enter {self.n}")
                return self.n
            def __exit__(self, *a):
                events.append(f"exit {self.n}")
                return False
        with T("a") as a, T("b") as b:
            events.append(f"body {a},{b}")
        self.assertEqual(events,
                         ["enter a", "enter b", "body a,b", "exit b", "exit a"])

    def test_suppress(self):
        class S:
            def __enter__(self): return self
            def __exit__(self, t, v, tb):
                return t is ValueError
        with S():
            raise ValueError("x")  # suppressed


unittest.main(globals())
