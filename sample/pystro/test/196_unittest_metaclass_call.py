import unittest


class MetaclassCallTest(unittest.TestCase):
    def test_singleton(self):
        class SM(type):
            _instances = {}
            def __call__(cls, *args, **kwargs):
                if cls not in cls._instances:
                    cls._instances[cls] = type.__call__(cls, *args, **kwargs)
                return cls._instances[cls]

        class S(metaclass=SM):
            def __init__(self, x): self.x = x

        a = S(1)
        b = S(2)
        self.assertIs(a, b)
        self.assertEqual(a.x, 1)  # First call wins.

    def test_intercepted_args(self):
        class TM(type):
            def __call__(cls, *args):
                # Reverse args before construction.
                return type.__call__(cls, *args[::-1])

        class P(metaclass=TM):
            def __init__(self, a, b): self.a = a; self.b = b

        p = P(1, 2)
        self.assertEqual((p.a, p.b), (2, 1))

    def test_class_attr_via_metaclass(self):
        # cls._instances must reach SM._instances even though it's
        # defined on the metaclass, not on the class itself.
        class SM(type):
            tag = "from-meta"

        class K(metaclass=SM):
            pass

        self.assertEqual(K.tag, "from-meta")


unittest.main(globals())
