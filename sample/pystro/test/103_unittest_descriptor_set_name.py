import unittest


class SetNameTest(unittest.TestCase):
    def test_basic(self):
        log = []
        class Desc:
            def __set_name__(self, owner, name):
                log.append((owner.__name__, name))
            def __get__(self, obj, owner=None):
                if obj is None: return self
                return obj.__dict__.get(self.__class__.__name__)
        class C:
            x = Desc()
            y = Desc()
        # Order may vary in our impl — sort.
        self.assertEqual(sorted([n for _, n in log]), ["x", "y"])
        for c, _ in log: self.assertEqual(c, "C")

    def test_validator_uses_name(self):
        class Validated:
            def __init__(self, predicate):
                self.predicate = predicate
                self.name = None
            def __set_name__(self, owner, name):
                self.name = name
            def __get__(self, obj, owner=None):
                if obj is None: return self
                return obj.__dict__.get(self.name)
            def __set__(self, obj, val):
                if not self.predicate(val):
                    raise TypeError(f"{self.name}: invalid")
                obj.__dict__[self.name] = val
        class P:
            age = Validated(lambda v: isinstance(v, int) and v >= 0)
            name = Validated(lambda v: isinstance(v, str))
        p = P()
        p.age = 30
        p.name = "Alice"
        self.assertEqual(p.age, 30)
        self.assertEqual(p.name, "Alice")
        with self.assertRaises(TypeError):
            p.age = -1


class ClassGetItemTest(unittest.TestCase):
    def test_basic(self):
        class C:
            def __class_getitem__(cls, item):
                return (cls.__name__, item)
        self.assertEqual(C[int], ("C", int))
        self.assertEqual(C["foo"], ("C", "foo"))


unittest.main(globals())
