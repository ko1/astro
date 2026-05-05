import unittest


class N:
    def __init__(self, v): self.v = v
    def __add__(self, o):  return N(self.v + (o.v if isinstance(o, N) else o))
    def __radd__(self, o): return N(o + self.v)
    def __sub__(self, o):  return N(self.v - (o.v if isinstance(o, N) else o))
    def __rsub__(self, o): return N(o - self.v)
    def __mul__(self, o):  return N(self.v * (o.v if isinstance(o, N) else o))
    def __pow__(self, o):  return N(self.v ** o)
    def __mod__(self, o):  return N(self.v % o)
    def __floordiv__(self, o): return N(self.v // o)
    def __truediv__(self, o):  return N(self.v / o)
    def __and__(self, o):  return N(self.v & o)
    def __or__(self, o):   return N(self.v | o)
    def __xor__(self, o):  return N(self.v ^ o)
    def __lshift__(self, o): return N(self.v << o)
    def __rshift__(self, o): return N(self.v >> o)
    def __invert__(self):  return N(~self.v)
    def __neg__(self):     return N(-self.v)
    def __pos__(self):     return self
    def __abs__(self):     return N(abs(self.v))
    def __int__(self):     return self.v
    def __float__(self):   return float(self.v)
    def __bool__(self):    return self.v != 0
    def __len__(self):     return self.v
    def __index__(self):   return self.v
    def __eq__(self, o):   return isinstance(o, N) and self.v == o.v
    def __hash__(self):    return hash(self.v)


class BinaryDunderTest(unittest.TestCase):
    def test_arith(self):
        self.assertEqual((N(5) + 3).v, 8)
        self.assertEqual((3 + N(5)).v, 8)
        self.assertEqual((N(5) - 1).v, 4)
        self.assertEqual((N(5) * 2).v, 10)
        self.assertEqual((N(5) ** 2).v, 25)
        self.assertEqual((N(5) % 2).v, 1)
        self.assertEqual((N(5) // 2).v, 2)

    def test_bitwise(self):
        self.assertEqual((N(5) & 3).v, 1)
        self.assertEqual((N(5) | 2).v, 7)
        self.assertEqual((N(5) ^ 1).v, 4)
        self.assertEqual((N(5) << 1).v, 10)
        self.assertEqual((N(5) >> 1).v, 2)

    def test_unary(self):
        self.assertEqual((~N(5)).v, -6)
        self.assertEqual((-N(5)).v, -5)
        self.assertEqual((+N(5)).v, 5)
        self.assertEqual(abs(N(-5)).v, 5)

    def test_conversions(self):
        self.assertEqual(int(N(5)), 5)
        self.assertEqual(float(N(5)), 5.0)
        self.assertTrue(bool(N(5)))
        self.assertFalse(bool(N(0)))
        self.assertEqual(len(N(5)), 5)


class IsinstanceObjectTest(unittest.TestCase):
    def test_object_universal(self):
        self.assertTrue(isinstance(1, object))
        self.assertTrue(isinstance("x", object))
        self.assertTrue(isinstance(None, object))
        self.assertTrue(isinstance([], object))

    def test_subclass_chain(self):
        self.assertTrue(issubclass(bool, int))
        self.assertTrue(issubclass(int, object))
        self.assertTrue(issubclass(IndexError, LookupError))
        self.assertTrue(issubclass(LookupError, Exception))
        self.assertTrue(issubclass(Exception, BaseException))


class GetattrUserDunderTest(unittest.TestCase):
    def test_getattr_fallback(self):
        class C:
            def __getattr__(self, name):
                return "miss:" + name
        self.assertEqual(C().nonexistent, "miss:nonexistent")


class CallableInstanceTest(unittest.TestCase):
    def test_call_kwargs(self):
        class P:
            def __init__(self, fn): self.fn = fn
            def __call__(self, *args, **kwargs):
                return self.fn(*args, **kwargs)
        p = P(int)
        self.assertEqual(p("ff", base=16), 255)


class LambdaDefaultScopeTest(unittest.TestCase):
    def test_default_uses_outer(self):
        def make_adders():
            return [(lambda x, i=i: x + i) for i in range(3)]
        adders = make_adders()
        self.assertEqual([f(0) for f in adders], [0, 1, 2])


class CallFromMethodKwargsTest(unittest.TestCase):
    def test_method_kwarg_call(self):
        # `int` resolves correctly inside method even with kwarg call.
        class A:
            def m(self):
                return int("ff", base=16)
        self.assertEqual(A().m(), 255)


class JsonIndentTest(unittest.TestCase):
    def test_indent(self):
        import json
        out = json.dumps({"a": 1}, indent=2)
        self.assertIn("\n", out)
        self.assertEqual(json.loads(out), {"a": 1})

    def test_sort_keys(self):
        import json
        out = json.dumps({"b": 2, "a": 1}, sort_keys=True)
        self.assertTrue(out.index("a") < out.index("b"))


class SysModuleTest(unittest.TestCase):
    def test_attrs(self):
        import sys
        self.assertEqual(sys.platform, "linux")
        self.assertGreater(sys.maxsize, 0)
        self.assertEqual(sys.byteorder, "little")


unittest.main(globals())
