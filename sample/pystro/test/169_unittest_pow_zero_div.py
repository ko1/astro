import unittest


class PowZeroNegTest(unittest.TestCase):
    def test_zero_neg_int(self):
        with self.assertRaises(ZeroDivisionError):
            0 ** -1

    def test_zero_neg_two(self):
        with self.assertRaises(ZeroDivisionError):
            0 ** -2

    def test_zero_zero(self):
        # 0**0 == 1 in Python
        self.assertEqual(0 ** 0, 1)

    def test_one_neg(self):
        # 1 ** -anything is 1.0 (float because exponent negative)
        self.assertEqual(1 ** -1, 1.0)

    def test_neg_int(self):
        self.assertEqual((-2) ** 3, -8)

    def test_int_neg_int(self):
        self.assertEqual(2 ** -1, 0.5)


class FloatDivByZeroTest(unittest.TestCase):
    def test_float_div(self):
        with self.assertRaises(ZeroDivisionError):
            1.0 / 0

    def test_float_floor_div(self):
        with self.assertRaises(ZeroDivisionError):
            1.0 // 0

    def test_float_mod(self):
        with self.assertRaises(ZeroDivisionError):
            1.0 % 0


unittest.main(globals())
