import unittest
import operator
import functools


class AttrgetterChainTest(unittest.TestCase):
    def test_dotted(self):
        class A: pass
        a = A()
        a.b = A()
        a.b.c = 42
        self.assertEqual(operator.attrgetter("b.c")(a), 42)

    def test_multi(self):
        class A: pass
        a = A()
        a.x = 1; a.y = 2
        self.assertEqual(operator.attrgetter("x", "y")(a), (1, 2))


class CustomDirTest(unittest.TestCase):
    def test_dir_override(self):
        class C:
            def __dir__(self): return ["foo", "bar", "baz"]
        self.assertEqual(sorted(dir(C())), ["bar", "baz", "foo"])


class PartialMethodTest(unittest.TestCase):
    def test_basic(self):
        class C:
            def greet(self, name, greeting):
                return f"{greeting}, {name}"
            say_hi = functools.partialmethod(greet, greeting="hi")
        c = C()
        self.assertEqual(c.say_hi("alice"), "hi, alice")


unittest.main(globals())
