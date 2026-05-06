import unittest


class DictViewSetOpsTest(unittest.TestCase):
    def test_keys_intersection(self):
        d1 = {"a": 1, "b": 2, "c": 3}
        d2 = {"b": 99, "c": 99, "d": 99}
        self.assertEqual(sorted(d1.keys() & d2.keys()), ["b", "c"])

    def test_keys_union(self):
        d1 = {"a": 1, "b": 2}
        d2 = {"b": 3, "c": 4}
        self.assertEqual(sorted(d1.keys() | d2.keys()), ["a", "b", "c"])

    def test_with_real_set(self):
        d = {"a": 1, "b": 2, "c": 3}
        s = {"b", "c", "d"}
        self.assertEqual(sorted(d.keys() & s), ["b", "c"])


unittest.main(globals())
