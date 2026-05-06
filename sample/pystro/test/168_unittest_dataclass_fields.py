import unittest
from dataclasses import dataclass, field, fields, asdict, astuple, is_dataclass, replace


class FieldsTest(unittest.TestCase):
    def test_field_objects_have_name(self):
        @dataclass
        class P:
            x: int
            y: int = 0
            z: list = field(default_factory=list)
        p = P(1, 2)
        names = [f.name for f in fields(p)]
        self.assertEqual(names, ["x", "y", "z"])

    def test_field_default(self):
        @dataclass
        class P:
            x: int = 5
        f = fields(P)[0]
        self.assertEqual(f.default, 5)

    def test_is_dataclass(self):
        @dataclass
        class P:
            x: int
        self.assertTrue(is_dataclass(P))
        self.assertTrue(is_dataclass(P(1)))

        class NotDC: pass
        self.assertFalse(is_dataclass(NotDC))

    def test_replace(self):
        @dataclass
        class P:
            x: int
            y: int = 0
        p = P(1, 2)
        p2 = replace(p, x=99)
        self.assertEqual(p2, P(99, 2))


class AsdictAstupleTest(unittest.TestCase):
    def test_asdict(self):
        @dataclass
        class P:
            x: int = 0
            y: list = field(default_factory=list)
        p = P(1, [2, 3])
        d = asdict(p)
        self.assertEqual(d, {"x": 1, "y": [2, 3]})
        # asdict returns a copy of mutable defaults
        d["y"].append(99)
        self.assertEqual(p.y, [2, 3])  # unchanged

    def test_astuple(self):
        @dataclass
        class P:
            x: int
            y: int
        self.assertEqual(astuple(P(1, 2)), (1, 2))


unittest.main(globals())
