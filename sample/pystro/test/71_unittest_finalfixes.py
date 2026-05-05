import unittest


class SetattrSliceBorrowTest(unittest.TestCase):
    """`setattr(x, name_from_split, v)` used to attach attr under wrong key
    when name was a slice-borrowed substring (without NUL terminator)."""
    def test_setattr_after_split(self):
        class A:
            pass
        a = A()
        for name in "x y z".split():
            setattr(a, name, 99)
        self.assertEqual(sorted(a.__dict__.keys()), ["x", "y", "z"])

    def test_class_loop_setattr(self):
        def make(field_str):
            fields = field_str.split()
            class A:
                def __init__(self, **kwargs):
                    for name in fields:
                        setattr(self, name, kwargs[name])
            return A
        A = make("x y z")
        a = A(x=1, y=2, z=3)
        self.assertEqual(a.x, 1)
        self.assertEqual(a.y, 2)
        self.assertEqual(a.z, 3)


class DunderArithFullTest(unittest.TestCase):
    def test_class_with_only_iadd(self):
        class IL:
            def __init__(self, v): self.v = v
            def __iadd__(self, o):
                self.v += o
                return self
        x = IL(10)
        x += 5
        self.assertEqual(x.v, 15)

    def test_pow_dunder(self):
        class N:
            def __init__(self, v): self.v = v
            def __pow__(self, o): return N(self.v ** o)
            def __eq__(self, o): return isinstance(o, N) and self.v == o.v
            def __hash__(self): return hash(self.v)
        self.assertEqual((N(3) ** 4).v, 81)


class IsinstanceFullTest(unittest.TestCase):
    def test_object_universal(self):
        for x in [1, "x", [], {}, set(), (), None, True, 1.5, 1j]:
            self.assertTrue(isinstance(x, object))

    def test_subclass_chain(self):
        self.assertTrue(issubclass(bool, int))
        self.assertTrue(issubclass(int, object))
        self.assertTrue(issubclass(str, object))
        self.assertTrue(issubclass(IndexError, LookupError))
        self.assertTrue(issubclass(ZeroDivisionError, ArithmeticError))
        self.assertTrue(issubclass(KeyboardInterrupt, BaseException))
        self.assertTrue(issubclass(GeneratorExit, BaseException))


class GetattrFallbackFullTest(unittest.TestCase):
    def test_getattr_with_existing_attr(self):
        class C:
            def __getattr__(self, name):
                return "miss:" + name
        c = C()
        c.real = 5
        self.assertEqual(c.real, 5)
        self.assertEqual(c.missing, "miss:missing")


class CallableInstanceKwargsTest(unittest.TestCase):
    def test_partial_with_int_kwargs(self):
        import functools
        f = functools.partial(int, base=16)
        self.assertEqual(f("ff"), 255)
        self.assertEqual(f("10"), 16)

    def test_class_method_with_kwargs_call(self):
        class T:
            def m(self):
                return int("ff", base=16)
        self.assertEqual(T().m(), 255)


unittest.main(globals())
