import unittest
from decimal import Decimal
from fractions import Fraction


class DecimalTest(unittest.TestCase):
    def test_construct(self):
        self.assertEqual(str(Decimal("1.5")), "1.5")
        self.assertEqual(str(Decimal("0.001")), "0.001")
        self.assertEqual(str(Decimal("-2.50")), "-2.50")
        self.assertEqual(str(Decimal(42)), "42")

    def test_arith(self):
        self.assertEqual(Decimal("1.5") + Decimal("2.25"), Decimal("3.75"))
        self.assertEqual(Decimal("10") - Decimal("3.5"), Decimal("6.5"))
        self.assertEqual(Decimal("1.5") * Decimal("2"), Decimal("3.00"))

    def test_compare(self):
        self.assertTrue(Decimal("1.5") < Decimal("1.6"))
        self.assertTrue(Decimal("1.5") <= Decimal("1.5"))
        self.assertTrue(Decimal("1.5") == Decimal("1.50"))
        self.assertFalse(Decimal("1.5") == Decimal("1.6"))


class FractionTest(unittest.TestCase):
    def test_construct(self):
        self.assertEqual(str(Fraction(1, 2)), "1/2")
        self.assertEqual(str(Fraction(2, 4)), "1/2")     # auto-reduced
        self.assertEqual(str(Fraction(0, 5)), "0")
        self.assertEqual(str(Fraction(-3, 6)), "-1/2")
        self.assertEqual(str(Fraction("3/7")), "3/7")
        self.assertEqual(str(Fraction(5)), "5")

    def test_add_sub(self):
        self.assertEqual(Fraction(1, 3) + Fraction(1, 6), Fraction(1, 2))
        self.assertEqual(Fraction(1, 2) - Fraction(1, 4), Fraction(1, 4))

    def test_mul_div(self):
        self.assertEqual(Fraction(2, 3) * Fraction(3, 4), Fraction(1, 2))
        self.assertEqual(Fraction(1, 2) / Fraction(1, 4), Fraction(2, 1))

    def test_compare(self):
        self.assertTrue(Fraction(1, 3) < Fraction(1, 2))
        self.assertTrue(Fraction(1, 2) == Fraction(2, 4))
        self.assertFalse(Fraction(1, 2) < Fraction(1, 3))

    def test_neg_abs(self):
        self.assertEqual(-Fraction(1, 3), Fraction(-1, 3))
        self.assertEqual(abs(Fraction(-2, 5)), Fraction(2, 5))

    def test_zero_div(self):
        with self.assertRaises(ZeroDivisionError):
            Fraction(1, 0)
        with self.assertRaises(ZeroDivisionError):
            Fraction(1, 2) / Fraction(0, 5)


unittest.main(globals())
