import unittest
from enum import Enum
from fractions import Fraction


class EnumMethodsTest(unittest.TestCase):
    def test_method_on_member(self):
        class Color(Enum):
            RED = 1
            GREEN = 2
            BLUE = 3
            def hex(self):
                return {Color.RED: "F00", Color.GREEN: "0F0", Color.BLUE: "00F"}[self]
        self.assertEqual(Color.RED.hex(), "F00")
        self.assertEqual(Color.BLUE.hex(), "00F")

    def test_str_method(self):
        class Tier(Enum):
            FREE = 1
            PAID = 2
            def label(self):
                return self.name + "-tier"
        self.assertEqual(Tier.FREE.label(), "FREE-tier")


class FractionFromFloatTest(unittest.TestCase):
    def test_basic(self):
        self.assertEqual(Fraction(0.5), Fraction(1, 2))
        self.assertEqual(Fraction(0.25), Fraction(1, 4))


unittest.main(globals())
