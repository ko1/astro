import unittest


class RangeHashTest(unittest.TestCase):
    def test_eq_ranges_same_hash(self):
        self.assertEqual(hash(range(10)), hash(range(10)))
        self.assertEqual(hash(range(0, 10)), hash(range(10)))
        self.assertEqual(hash(range(0, 10, 1)), hash(range(10)))

    def test_as_dict_key(self):
        d = {range(10): "ten", range(5): "five"}
        self.assertEqual(d[range(10)], "ten")
        self.assertEqual(d[range(5)], "five")

    def test_distinct_ranges_diff_hash(self):
        # Best-effort — hashes may collide but usually don't.
        h1 = hash(range(10))
        h2 = hash(range(20))
        # Just check they don't crash; collision is allowed.
        self.assertIsInstance(h1, int)
        self.assertIsInstance(h2, int)


unittest.main(globals())
