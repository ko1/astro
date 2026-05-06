import unittest


class TypeTest(unittest.TestCase):
    def test_type_names(self):
        self.assertEqual(type(42).__name__, "int")
        self.assertEqual(type([]).__name__, "list")
        self.assertEqual(type({}).__name__, "dict")
        self.assertEqual(type(set()).__name__, "set")
        self.assertEqual(type(()).__name__, "tuple")
        self.assertEqual(type("").__name__, "str")
        self.assertEqual(type(b"").__name__, "bytes")
        self.assertEqual(type(None).__name__, "NoneType")
        self.assertEqual(type(True).__name__, "bool")

    def test_user_class_name(self):
        class C: pass
        self.assertEqual(type(C()).__name__, "C")
        self.assertEqual(type(C).__name__, "type")


class IsinstanceTest(unittest.TestCase):
    def test_chain(self):
        class A: pass
        class B(A): pass
        class C(B): pass
        c = C()
        self.assertTrue(isinstance(c, C))
        self.assertTrue(isinstance(c, B))
        self.assertTrue(isinstance(c, A))
        self.assertTrue(isinstance(c, object))

    def test_tuple(self):
        self.assertTrue(isinstance(1, (int, str)))
        self.assertTrue(isinstance("x", (int, str)))
        self.assertFalse(isinstance(1.5, (int, str)))


class IssubclassTest(unittest.TestCase):
    def test_chain(self):
        class A: pass
        class B(A): pass
        self.assertTrue(issubclass(B, A))
        self.assertFalse(issubclass(A, B))
        self.assertTrue(issubclass(B, B))

    def test_object_root(self):
        class A: pass
        self.assertTrue(issubclass(A, object))
        self.assertTrue(issubclass(int, object))


class HasattrTest(unittest.TestCase):
    def test_hasattr(self):
        class C:
            x = 1
            def m(self): pass
        c = C()
        self.assertTrue(hasattr(c, "x"))
        self.assertTrue(hasattr(c, "m"))
        self.assertFalse(hasattr(c, "y"))

    def test_getattr_default(self):
        class C: pass
        self.assertEqual(getattr(C(), "z", "DEFAULT"), "DEFAULT")


class MROTest(unittest.TestCase):
    def test_mro(self):
        class A: pass
        class B(A): pass
        class C(A): pass
        class D(B, C): pass
        self.assertEqual([c.__name__ for c in D.__mro__],
                         ["D", "B", "C", "A", "object"])


class CallableTest(unittest.TestCase):
    def test_basic(self):
        self.assertTrue(callable(lambda: 0))
        self.assertTrue(callable(int))
        self.assertFalse(callable(42))

    def test_call_method(self):
        class Cb:
            def __call__(self, x): return x
        self.assertTrue(callable(Cb()))


class InitSubclassTest(unittest.TestCase):
    def test_collects(self):
        class Plugin:
            registered = []
            def __init_subclass__(cls, **kw):
                Plugin.registered.append(cls.__name__)
        class P1(Plugin): pass
        class P2(Plugin): pass
        self.assertEqual(Plugin.registered, ["P1", "P2"])

    def test_kwargs(self):
        class Tagged:
            def __init_subclass__(cls, tag=None, **kw):
                cls.tag = tag
        class TA(Tagged, tag="A"): pass
        class TB(Tagged, tag="B"): pass
        self.assertEqual((TA.tag, TB.tag), ("A", "B"))


class BasesTest(unittest.TestCase):
    def test_bases(self):
        class A: pass
        class B(A): pass
        self.assertEqual([c.__name__ for c in B.__bases__], ["A"])


unittest.main(globals())
