import unittest


class DescriptorTest(unittest.TestCase):
    def test_set_name(self):
        class Doubler:
            def __set_name__(self, owner, name):
                self.attr = "_" + name
            def __get__(self, obj, owner=None):
                if obj is None: return self
                return getattr(obj, self.attr, 0)
            def __set__(self, obj, value):
                setattr(obj, self.attr, value * 2)

        class C:
            x = Doubler()
            y = Doubler()

        c = C()
        c.x = 5
        c.y = 7
        self.assertEqual(c.x, 10)
        self.assertEqual(c.y, 14)

    def test_non_data_descriptor(self):
        class ND:
            def __get__(self, obj, owner=None): return "default"

        class C:
            x = ND()

        c = C()
        self.assertEqual(c.x, "default")
        c.__dict__["x"] = "instance"
        self.assertEqual(c.x, "instance")  # instance dict wins for non-data desc


class BoundMethodEqTest(unittest.TestCase):
    def test_same_method(self):
        class M:
            def f(self): pass
        m = M()
        self.assertEqual(m.f, m.f)
        self.assertNotEqual(m.f, M().f)  # different instance

    def test_method_in_set(self):
        class M:
            def f(self): pass
            def g(self): pass
        m = M()
        s = {m.f, m.f, m.g}
        # CPython: m.f hashable; same self+func collapse to one entry.
        self.assertEqual(len(s), 2)


class GetattrFallbackTest(unittest.TestCase):
    def test_getattr_called(self):
        class C:
            def __getattr__(self, name):
                if name.startswith("_"): raise AttributeError(name)
                return "GA_" + name
        c = C()
        self.assertEqual(c.foo, "GA_foo")
        c.x = 1
        self.assertEqual(c.x, 1)
        with self.assertRaises(AttributeError):
            c._private


unittest.main(globals())
