import unittest


class SetCompareTest(unittest.TestCase):
    def test_subset(self):
        self.assertTrue({1, 2} <= {1, 2, 3})
        self.assertFalse({1, 2, 4} <= {1, 2, 3})

    def test_strict_subset(self):
        self.assertTrue({1, 2} < {1, 2, 3})
        self.assertFalse({1, 2} < {1, 2})    # equal, not strict
        self.assertTrue({1, 2} <= {1, 2})

    def test_superset(self):
        self.assertTrue({1, 2, 3} >= {1, 2})
        self.assertFalse({1, 2} >= {1, 2, 3})

    def test_strict_superset(self):
        self.assertTrue({1, 2, 3} > {1, 2})
        self.assertFalse({1, 2} > {1, 2})

    def test_set_vs_frozenset(self):
        self.assertEqual({1, 2, 3}, frozenset([1, 2, 3]))
        self.assertTrue({1, 2} <= frozenset([1, 2, 3]))


unittest.main(globals())
