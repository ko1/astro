import unittest
from enum import Enum, IntEnum, auto


class EnumIterTest(unittest.TestCase):
    def test_len(self):
        class Color(Enum):
            RED = 1
            GREEN = 2
            BLUE = 3
        self.assertEqual(len(Color), 3)

    def test_iter(self):
        class Color(Enum):
            RED = 1
            GREEN = 2
            BLUE = 3
        names = [m.name for m in Color]
        self.assertEqual(names, ["RED", "GREEN", "BLUE"])

    def test_contains(self):
        class Color(Enum):
            RED = 1
            GREEN = 2
        self.assertIn(Color.RED, Color)

    def test_auto(self):
        class Side(Enum):
            LEFT = auto()
            RIGHT = auto()
        self.assertEqual(Side.LEFT.value, 1)
        self.assertEqual(Side.RIGHT.value, 2)

    def test_int_enum_value(self):
        class Status(IntEnum):
            OK = 200
            ERR = 500
        self.assertEqual(Status.OK.value, 200)


unittest.main(globals())
