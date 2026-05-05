import unittest


class FuncAttrTest(unittest.TestCase):
    def test_defaults(self):
        def f(a, b=10, c=20): return a + b + c
        self.assertEqual(f.__defaults__, (10, 20))

    def test_no_defaults(self):
        def f(a, b): return a + b
        self.assertIsNone(f.__defaults__)

    def test_kwdefaults(self):
        def f(a, *, b=10, c=20): return a + b + c
        kw = f.__kwdefaults__
        self.assertEqual(kw["b"], 10)
        self.assertEqual(kw["c"], 20)

    def test_user_attr(self):
        def f(): pass
        f.color = "red"
        self.assertEqual(f.color, "red")


class ImportlibTest(unittest.TestCase):
    def test_import_module(self):
        import importlib
        m = importlib.import_module("math")
        self.assertEqual(m.pi, m.pi)

    def test_dunder_import(self):
        m = __import__("json")
        self.assertEqual(m.dumps([1, 2]), "[1, 2]")


unittest.main(globals())
