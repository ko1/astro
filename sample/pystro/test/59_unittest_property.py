# Property descriptors: getter, setter, class-body name resolution.

import unittest


class PropertyTest(unittest.TestCase):
    def test_getter_only(self):
        class P:
            @property
            def x(self):
                return 42
        p = P()
        self.assertEqual(p.x, 42)

    def test_getter_setter(self):
        class P:
            @property
            def x(self):
                return self._x
            @x.setter
            def x(self, v):
                self._x = v + 100
        p = P()
        p.x = 5
        self.assertEqual(p._x, 105)
        self.assertEqual(p.x, 105)

    def test_setter_runs(self):
        class C:
            log = []
            @property
            def v(self):
                return self._v
            @v.setter
            def v(self, val):
                C.log.append(val)
                self._v = val * 2
        c = C()
        c.v = 7
        c.v = 9
        self.assertEqual(C.log, [7, 9])
        self.assertEqual(c.v, 18)

    def test_no_setter_raises(self):
        class C:
            @property
            def x(self):
                return 1
        c = C()
        with self.assertRaises(AttributeError):
            c.x = 5

    def test_class_body_name_lookup(self):
        # Class body should be able to read attributes defined earlier.
        class C:
            a = 5
            b = a + 1
        self.assertEqual(C.a, 5)
        self.assertEqual(C.b, 6)

    def test_property_attrs(self):
        class C:
            @property
            def x(self):
                return 1
        self.assertIsNotNone(C.x.fget)
        # No setter installed yet.
        self.assertIs(C.x.fset, None)

    def test_class_docstring(self):
        class C:
            """Hello there."""
            pass
        self.assertEqual(C.__doc__, "Hello there.")

    def test_class_no_docstring(self):
        class C:
            pass
        self.assertIsNone(C.__doc__)

    def test_chained_decorators(self):
        # Pre-defined decorator that wraps to add 1.
        class C:
            def deco(fn):
                def wrapper(self):
                    return fn(self) + 1
                return wrapper
            @deco
            def m(self):
                return 10
        c = C()
        self.assertEqual(c.m(), 11)


unittest.main(globals())
