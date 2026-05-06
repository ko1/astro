import unittest
from collections import namedtuple


class DictViewSetOpsExtraTest(unittest.TestCase):
    def test_keys_difference(self):
        d1 = {"a": 1, "b": 2, "c": 3}
        d2 = {"b": 99, "c": 99, "d": 99}
        self.assertEqual(sorted(d1.keys() - d2.keys()), ["a"])

    def test_keys_xor(self):
        d1 = {"a": 1, "b": 2, "c": 3}
        d2 = {"b": 99, "c": 99, "d": 99}
        self.assertEqual(sorted(d1.keys() ^ d2.keys()), ["a", "d"])


class NamedtupleDefaultsTest(unittest.TestCase):
    def test_basic(self):
        P = namedtuple("P", ["x", "y", "z"], defaults=[10, 20])
        self.assertEqual((P(1).x, P(1).y, P(1).z), (1, 10, 20))
        self.assertEqual((P(1, 2).x, P(1, 2).y, P(1, 2).z), (1, 2, 20))

    def test_kwargs_with_default(self):
        P = namedtuple("P", ["x", "y"], defaults=[99])
        p = P(x=1)
        self.assertEqual((p.x, p.y), (1, 99))


unittest.main(globals())
