import unittest


class NewDunderTest(unittest.TestCase):
    def test_singleton(self):
        class Singleton:
            _instance = None
            def __new__(cls):
                if cls._instance is None:
                    cls._instance = object.__new__(cls)
                return cls._instance
        a = Singleton()
        b = Singleton()
        self.assertIs(a, b)

    def test_class_attr_None(self):
        # Used to be indistinguishable from "missing".
        class C:
            x = None
        self.assertIsNone(C.x)

    def test_built_in_subclass_still_works(self):
        class M(list):
            pass
        m = M([1, 2, 3])
        m.append(4)
        self.assertEqual(len(m), 4)
        self.assertEqual(m[0], 1)


unittest.main(globals())
