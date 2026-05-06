import unittest


class RangeBoolTest(unittest.TestCase):
    def test_empty_falsy(self):
        self.assertFalse(bool(range(0)))
        self.assertFalse(bool(range(5, 5)))
        self.assertFalse(bool(range(10, 5)))  # positive step, decreasing → empty

    def test_non_empty_truthy(self):
        self.assertTrue(bool(range(5)))
        self.assertTrue(bool(range(1, 2)))
        self.assertTrue(bool(range(10, 5, -1)))  # negative step, decreasing → non-empty


class RoundBankerTest(unittest.TestCase):
    def test_half_to_even(self):
        # CPython rounds half to even (banker's rounding).
        self.assertEqual(round(0.5), 0)
        self.assertEqual(round(1.5), 2)
        self.assertEqual(round(2.5), 2)
        self.assertEqual(round(3.5), 4)
        self.assertEqual(round(4.5), 4)
        self.assertEqual(round(-0.5), 0)
        self.assertEqual(round(-1.5), -2)

    def test_int_args(self):
        # round() on an int is a no-op.
        self.assertEqual(round(10), 10)

    def test_negative_ndigits(self):
        self.assertEqual(round(1234, -2), 1200)


unittest.main(globals())
