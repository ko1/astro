import unittest
import math


class NanIdentityInListTest(unittest.TestCase):
    def test_in_uses_identity(self):
        nan = math.nan
        self.assertIn(nan, [nan])

    def test_count_uses_identity(self):
        nan = math.nan
        self.assertEqual([nan].count(nan), 1)

    def test_index_uses_identity(self):
        nan = math.nan
        self.assertEqual([nan].index(nan), 0)

    def test_nan_eq_nan_still_false(self):
        nan = math.nan
        self.assertFalse(nan == nan)


unittest.main(globals())
