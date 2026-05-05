import unittest


class WalrusInLambdaTest(unittest.TestCase):
    def test_walrus_in_listcomp_in_lambda(self):
        f = lambda data: [y for x in data if (y := x * 2) > 4]
        self.assertEqual(f([1, 2, 3, 4, 5]), [6, 8, 10])

    def test_walrus_in_method(self):
        class C:
            def m(self, data):
                return [y for x in data if (y := x * 3) > 6]
        self.assertEqual(C().m([1, 2, 3, 4]), [9, 12])


class BytesMethodsTest(unittest.TestCase):
    def test_join(self):
        self.assertEqual(b",".join([b"a", b"b", b"c"]), b"a,b,c")

    def test_count(self):
        self.assertEqual(b"abca".count(b"a"), 2)
        self.assertEqual(b"abc".count(b"x"), 0)

    def test_upper_lower(self):
        self.assertEqual(b"Hello".upper(), b"HELLO")
        self.assertEqual(b"Hello".lower(), b"hello")

    def test_strip(self):
        self.assertEqual(b"  hi  ".strip(), b"hi")
        self.assertEqual(b"  hi  ".lstrip(), b"hi  ")
        self.assertEqual(b"  hi  ".rstrip(), b"  hi")


class DictUpdateKwargsTest(unittest.TestCase):
    def test_kwargs_only(self):
        d = {"x": 1}
        d.update(y=2, z=3)
        self.assertEqual(d, {"x": 1, "y": 2, "z": 3})

    def test_arg_and_kwargs(self):
        d = {}
        d.update({"a": 1}, b=2)
        self.assertEqual(d, {"a": 1, "b": 2})


class StrSplitlinesKeepEndsTest(unittest.TestCase):
    def test_default(self):
        self.assertEqual("a\nb\nc".splitlines(), ["a", "b", "c"])
        self.assertEqual("a\nb\n".splitlines(), ["a", "b"])

    def test_keepends(self):
        self.assertEqual("a\nb\n".splitlines(True), ["a\n", "b\n"])


class TypesModuleTest(unittest.TestCase):
    def test_function_type(self):
        import types
        self.assertEqual(types.FunctionType.__name__, "function")

    def test_simple_namespace(self):
        import types
        ns = types.SimpleNamespace(x=1, y=2)
        self.assertEqual(ns.x, 1)
        self.assertEqual(ns.y, 2)


class BoundedStringFunctionsTest(unittest.TestCase):
    def test_find_with_range(self):
        self.assertEqual("aaba".find("a", 1, 3), 1)
        self.assertEqual("aaba".find("a", 2, 3), -1)
        self.assertEqual("aaba".find("a", 2), 3)


unittest.main(globals())
