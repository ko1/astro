import unittest
import contextlib


class ContextManagerTest(unittest.TestCase):
    def test_basic(self):
        @contextlib.contextmanager
        def cm(name):
            yield "X-" + name
        with cm("y") as v:
            self.assertEqual(v, "X-y")

    def test_finally_runs(self):
        events = []
        @contextlib.contextmanager
        def cm():
            events.append("enter")
            try:
                yield
            finally:
                events.append("exit")
        with cm():
            events.append("body")
        self.assertEqual(events, ["enter", "body", "exit"])

    def test_handles_exception(self):
        # Generator catches the thrown exception and swallows it.
        captured = []
        @contextlib.contextmanager
        def cm(L):
            try:
                yield L
            except Exception as e:
                L.append(str(e))
        with cm(captured):
            raise ValueError("bad")
        self.assertEqual(captured, ["bad"])

    def test_propagates_exception(self):
        # Generator does NOT handle the thrown exception → propagates.
        @contextlib.contextmanager
        def cm():
            yield
        with self.assertRaises(ValueError):
            with cm():
                raise ValueError("oops")


class SuppressTest(unittest.TestCase):
    def test_one(self):
        with contextlib.suppress(ValueError):
            raise ValueError("hi")

    def test_multi(self):
        with contextlib.suppress(ValueError, TypeError):
            raise TypeError("hi")

    def test_no_match(self):
        with self.assertRaises(KeyError):
            with contextlib.suppress(ValueError):
                raise KeyError("k")


class ExitStackTest(unittest.TestCase):
    def test_lifo_order(self):
        events = []
        class CM:
            def __init__(self, name): self.name = name
            def __enter__(self): events.append(f"enter {self.name}"); return self.name
            def __exit__(self, *a): events.append(f"exit {self.name}"); return False

        with contextlib.ExitStack() as st:
            a = st.enter_context(CM("a"))
            b = st.enter_context(CM("b"))
            self.assertEqual((a, b), ("a", "b"))
        self.assertEqual(events,
                         ["enter a", "enter b", "exit b", "exit a"])

    def test_callback(self):
        events = []
        with contextlib.ExitStack() as st:
            st.callback(lambda: events.append("cb"))
            events.append("body")
        self.assertEqual(events, ["body", "cb"])


class ClosingTest(unittest.TestCase):
    def test_close(self):
        events = []
        class O:
            def close(self): events.append("closed")
        with contextlib.closing(O()):
            events.append("body")
        self.assertEqual(events, ["body", "closed"])


class NullContextTest(unittest.TestCase):
    def test_value(self):
        with contextlib.nullcontext("V") as v:
            self.assertEqual(v, "V")

    def test_no_arg(self):
        with contextlib.nullcontext() as v:
            self.assertIsNone(v)


unittest.main(globals())
