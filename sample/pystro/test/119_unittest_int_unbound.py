import unittest


class IntUnboundMethodTest(unittest.TestCase):
    def test_bit_length(self):
        self.assertEqual(int.bit_length(255), 8)

    def test_to_bytes(self):
        self.assertEqual(int.to_bytes(255, 2, "big"), b"\x00\xff")

    def test_hasattr(self):
        self.assertTrue(hasattr(int, "bit_length"))
        self.assertTrue(hasattr(int, "to_bytes"))


class FloatUnboundMethodTest(unittest.TestCase):
    def test_is_integer(self):
        self.assertTrue(float.is_integer(3.0))
        self.assertFalse(float.is_integer(3.14))

    def test_as_integer_ratio(self):
        self.assertEqual(float.as_integer_ratio(0.5), (1, 2))


unittest.main(globals())
