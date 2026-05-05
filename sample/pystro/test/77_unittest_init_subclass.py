import unittest


class InitSubclassTest(unittest.TestCase):
    def test_basic(self):
        class Parent:
            subs = []
            def __init_subclass__(cls, **kw):
                Parent.subs.append(cls.__name__)
        class A(Parent): pass
        class B(Parent): pass
        self.assertEqual(Parent.subs, ["A", "B"])

    def test_inherited(self):
        log = []
        class Root:
            def __init_subclass__(cls, **kw):
                log.append(cls.__name__)
        class Mid(Root): pass
        class Leaf(Mid): pass
        # __init_subclass__ should be called for every subclass.
        self.assertIn("Mid", log)
        self.assertIn("Leaf", log)


class FormatNestedTest(unittest.TestCase):
    def test_dynamic_width(self):
        n = 5
        self.assertEqual(f"{42:0{n}d}", "00042")

    def test_dynamic_precision(self):
        p = 3
        self.assertEqual(f"{3.14159:.{p}f}", "3.142")


unittest.main(globals())
