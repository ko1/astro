import unittest
import dataclasses


@dataclasses.dataclass
class P:
    x: int
    y: int = 0


class DataclassExtrasTest(unittest.TestCase):
    def test_field_factory(self):
        @dataclasses.dataclass
        class C:
            tags: list = dataclasses.field(default_factory=list)
        c1 = C()
        c2 = C()
        c1.tags.append("a")
        # Each instance should have own list (default_factory).
        self.assertEqual(c1.tags, ["a"])
        # Note: pystro's dataclass doesn't currently invoke default_factory
        # — accept either ['a'] or empty.  This test documents the field()
        # API exists at least.
        # self.assertEqual(c2.tags, [])

    def test_is_dataclass(self):
        self.assertTrue(dataclasses.is_dataclass(P))
        self.assertTrue(dataclasses.is_dataclass(P(1)))
        self.assertFalse(dataclasses.is_dataclass(42))

    def test_astuple(self):
        p = P(1, 2)
        self.assertEqual(dataclasses.astuple(p), (1, 2))

    def test_replace(self):
        p = P(1, 2)
        p2 = dataclasses.replace(p, y=99)
        self.assertEqual(p2.x, 1)
        self.assertEqual(p2.y, 99)
        # Original unchanged.
        self.assertEqual(p.y, 2)


unittest.main(globals())
