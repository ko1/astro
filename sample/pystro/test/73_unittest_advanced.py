import unittest


class MetaclassInheritTest(unittest.TestCase):
    def test_metaclass_applied_to_subclass(self):
        log = []
        class M(type):
            def __new__(meta, name, bases, attrs):
                log.append(name)
                return type(name, bases, attrs)
        class A(metaclass=M):
            pass
        class B(A):
            pass
        class C(B):
            pass
        self.assertIn("A", log)
        self.assertIn("B", log)
        self.assertIn("C", log)


class AnnotationsTest(unittest.TestCase):
    def test_class_annotations_dict(self):
        class P:
            x: int
            y: int
        self.assertEqual(set(P.__annotations__.keys()), {"x", "y"})

    def test_dataclass_via_annotations(self):
        from dataclasses import dataclass
        @dataclass
        class P:
            x: int
            y: int
        p = P(1, 2)
        self.assertEqual(p.x, 1)
        self.assertEqual(p.y, 2)


class EnumProperTest(unittest.TestCase):
    def test_enum_class_syntax(self):
        from enum import Enum
        class Color(Enum):
            RED = 1
            GREEN = 2
            BLUE = 3
        self.assertEqual(Color.RED.value, 1)
        self.assertEqual(Color.RED.name, "RED")
        self.assertEqual(Color.GREEN.value, 2)
        self.assertEqual(len(Color._members_), 3)


class HashConsistencyTest(unittest.TestCase):
    def test_int_float_bool(self):
        self.assertEqual(hash(1), hash(1.0))
        self.assertEqual(hash(True), hash(1))
        self.assertEqual(hash(False), hash(0))
        self.assertEqual(hash(0), hash(0.0))


class IntFromFloatErrorTest(unittest.TestCase):
    def test_nan(self):
        with self.assertRaises(ValueError):
            int(float("nan"))

    def test_inf(self):
        with self.assertRaises(OverflowError):
            int(float("inf"))


class FrozensetUnion(unittest.TestCase):
    def test_with_set(self):
        f = frozenset([1, 2])
        s = {2, 3}
        # frozenset | set
        result = f | s
        self.assertEqual(result, {1, 2, 3})


class ListLambda(unittest.TestCase):
    def test_lambda_default_in_listcomp(self):
        # `lambda x, i=i:` pattern for closure over loop var.
        adders = [(lambda x, i=i: x + i) for i in range(3)]
        self.assertEqual([f(10) for f in adders], [10, 11, 12])


unittest.main(globals())
