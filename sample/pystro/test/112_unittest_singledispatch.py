import unittest
import functools


class SingleDispatchTest(unittest.TestCase):
    def test_basic(self):
        @functools.singledispatch
        def proc(arg):
            return f"default-{arg}"
        @proc.register(int)
        def _(arg):
            return f"int-{arg}"
        @proc.register(str)
        def _(arg):
            return f"str-{arg}"

        self.assertEqual(proc(42), "int-42")
        self.assertEqual(proc("hi"), "str-hi")
        self.assertEqual(proc([1]), "default-[1]")

    def test_registry(self):
        @functools.singledispatch
        def f(x): return "default"
        @f.register(int)
        def _(x): return "int"
        self.assertIn(int, f.registry)


unittest.main(globals())
