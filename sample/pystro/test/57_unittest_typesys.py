# Type system: type-as-class, isinstance(int, type), introspection.

import unittest


class TypeSystemTest(unittest.TestCase):
    def test_type_is_class(self):
        # `int`, `str`, etc. are now real class objects.
        self.assertIs(type(5), int)
        self.assertIs(type("x"), str)
        self.assertIs(type([]), list)
        self.assertIs(type({}), dict)
        self.assertIs(type(()), tuple)
        self.assertIs(type(set()), set)
        self.assertIs(type(1.5), float)
        self.assertIs(type(True), bool)

    def test_class_of_class(self):
        # type(int) is type — int IS an instance of type.
        self.assertTrue(isinstance(int, type))
        self.assertTrue(isinstance(str, type))
        self.assertTrue(isinstance(list, type))
        # type itself is a class too.
        self.assertTrue(isinstance(type, type))

    def test_class_introspection(self):
        class C:
            pass
        self.assertEqual(C.__name__, "C")
        self.assertEqual(C.__module__, "__main__")
        self.assertIsNone(C.__doc__)
        # Bases / MRO.
        class D(C):
            pass
        self.assertIn(C, D.__bases__)
        self.assertIn(D, D.__mro__)
        self.assertIn(C, D.__mro__)

    def test_class_dict(self):
        class P:
            x = 5
            def m(self): return self.x
        d = P.__dict__
        self.assertIn("m", d)

    def test_instance_class(self):
        class C: pass
        c = C()
        self.assertIs(c.__class__, C)
        self.assertEqual(c.__dict__, {})
        c.x = 99
        self.assertEqual(c.__dict__, {"x": 99})

    def test_function_meta(self):
        def f():
            return 5
        self.assertEqual(f.__name__, "f")
        self.assertEqual(f.__qualname__, "f")
        self.assertEqual(f.__module__, "__main__")
        self.assertEqual(f.__annotations__, {})

    def test_dunder_globals(self):
        self.assertEqual(__name__, "__main__")

    def test_import_func(self):
        m = __import__("math")
        self.assertEqual(m.pi, 3.141592653589793)

    def test_subclass_of_int(self):
        # `class M(int):` parses and creates a class.
        class M(int):
            def double(self):
                return self.x if False else 99
        # M is a class.
        self.assertTrue(isinstance(M, type))
        # M's bases include int.
        self.assertIn(int, M.__bases__)
        # M is a subclass of int.
        self.assertTrue(issubclass(M, int))


unittest.main(globals())
