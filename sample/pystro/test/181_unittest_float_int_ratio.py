import unittest
import math


class FloatRatioTest(unittest.TestCase):
    def test_zero(self):
        self.assertEqual((0.0).as_integer_ratio(), (0, 1))
        self.assertEqual((-0.0).as_integer_ratio(), (0, 1))

    def test_half(self):
        self.assertEqual((0.5).as_integer_ratio(), (1, 2))

    def test_int_value(self):
        self.assertEqual((3.0).as_integer_ratio(), (3, 1))
        self.assertEqual((-3.0).as_integer_ratio(), (-3, 1))

    def test_inf_raises(self):
        with self.assertRaises(OverflowError):
            math.inf.as_integer_ratio()

    def test_nan_raises(self):
        with self.assertRaises((ValueError, OverflowError)):
            math.nan.as_integer_ratio()


class IntBitTest(unittest.TestCase):
    def test_bit_count(self):
        self.assertEqual((7).bit_count(), 3)
        self.assertEqual((255).bit_count(), 8)
        self.assertEqual((-7).bit_count(), 3)  # CPython: count |x|

    def test_bit_length(self):
        self.assertEqual((0).bit_length(), 0)
        self.assertEqual((7).bit_length(), 3)
        self.assertEqual((255).bit_length(), 8)


unittest.main(globals())
