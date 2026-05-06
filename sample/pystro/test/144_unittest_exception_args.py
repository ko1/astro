import unittest


class ExceptionArgsTest(unittest.TestCase):
    def test_one_arg(self):
        e = ValueError("msg")
        self.assertEqual(e.args, ("msg",))
        self.assertEqual(str(e), "msg")
        self.assertEqual(repr(e), "ValueError('msg')")

    def test_two_args(self):
        e = ValueError("msg", 42)
        self.assertEqual(e.args, ("msg", 42))
        self.assertEqual(str(e), "('msg', 42)")
        self.assertEqual(repr(e), "ValueError('msg', 42)")

    def test_no_arg(self):
        e = ValueError()
        self.assertEqual(e.args, ())
        self.assertEqual(str(e), "")
        self.assertEqual(repr(e), "ValueError()")

    def test_args_setter(self):
        e = ValueError("x")
        e.args = (1, 2, 3)
        self.assertEqual(str(e), "(1, 2, 3)")

    def test_custom_three_arg(self):
        class CustomEx(Exception): pass
        e = CustomEx("a", "b", "c")
        self.assertEqual(e.args, ("a", "b", "c"))
        self.assertEqual(str(e), "('a', 'b', 'c')")

    def test_custom_str(self):
        class StrErr(Exception):
            def __init__(self, x): self.x = x; super().__init__(x)
            def __str__(self): return f"<x={self.x}>"
        try:
            raise StrErr(7)
        except StrErr as e:
            self.assertEqual(str(e), "<x=7>")


class ExceptionChainTest(unittest.TestCase):
    def test_implicit_context(self):
        try:
            try:
                raise ValueError("a")
            except:
                raise RuntimeError("b")
        except RuntimeError as e:
            self.assertIsInstance(e.__context__, ValueError)

    def test_re_raise(self):
        def inner():
            try:
                raise ValueError("x")
            except:
                raise
        with self.assertRaises(ValueError):
            inner()

    def test_finally_returns(self):
        def f():
            try:
                raise ValueError("err")
            finally:
                return "ok"
        self.assertEqual(f(), "ok")


class WithStmtTest(unittest.TestCase):
    def test_suppress(self):
        class Suppress:
            def __enter__(self): return self
            def __exit__(self, t, v, tb): return True
        with Suppress():
            raise ValueError("x")
        # If we reach here, the exception was suppressed.

    def test_propagate(self):
        events = []
        class CM:
            def __enter__(self): events.append("enter"); return self
            def __exit__(self, t, v, tb):
                events.append("exit")
                return False
        with self.assertRaises(ValueError):
            with CM():
                raise ValueError("inner")
        self.assertEqual(events, ["enter", "exit"])


unittest.main(globals())
