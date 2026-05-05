# Adapted from CPython Lib/test/test_int.py — int conversions, edge
# cases, base parsing.  Filtered for pystro's subset of features.

import unittest


class IntTest(unittest.TestCase):
    def test_basic(self):
        self.assertEqual(int(314), 314)
        self.assertEqual(int(3.14), 3)
        self.assertEqual(int(-3.14), -3)
        self.assertEqual(int(3.9), 3)
        self.assertEqual(int(-3.9), -3)
        self.assertEqual(int(3.5), 3)
        self.assertEqual(int(-3.5), -3)
        self.assertEqual(int("-3"), -3)
        self.assertEqual(int(" -3 "), -3)

    def test_base16(self):
        self.assertEqual(int("10", 16), 16)
        self.assertEqual(int("ff", 16), 255)
        self.assertEqual(int("0xff", 16), 255)
        self.assertEqual(int("FF", 16), 255)

    def test_base2(self):
        self.assertEqual(int("1010", 2), 10)
        self.assertEqual(int("0", 2), 0)
        self.assertEqual(int("1", 2), 1)

    def test_base8(self):
        self.assertEqual(int("777", 8), 511)
        self.assertEqual(int("10", 8), 8)

    def test_base0_prefix(self):
        self.assertEqual(int("0xff", 0), 255)
        self.assertEqual(int("0b101", 0), 5)
        self.assertEqual(int("0o17", 0), 15)
        self.assertEqual(int("123", 0), 123)

    def test_invalid(self):
        try:
            int("abc")
            self.fail("expected ValueError")
        except ValueError:
            pass
        try:
            int("")
            self.fail("expected ValueError")
        except ValueError:
            pass

    def test_bool_is_int(self):
        self.assertEqual(int(True), 1)
        self.assertEqual(int(False), 0)
        self.assertEqual(True + True, 2)
        self.assertEqual(True * 5, 5)

    def test_negative(self):
        self.assertEqual(-5, 0 - 5)
        self.assertEqual(--5, 5)
        self.assertEqual(abs(-100), 100)

    def test_div(self):
        self.assertEqual(10 // 3, 3)
        self.assertEqual(-10 // 3, -4)
        self.assertEqual(10 % 3, 1)
        self.assertEqual(-10 % 3, 2)

    def test_pow(self):
        self.assertEqual(2 ** 10, 1024)
        self.assertEqual(2 ** 30, 1073741824)
        self.assertEqual(pow(2, 10), 1024)

    def test_bignum(self):
        big = 1
        for _ in range(100):
            big = big * 2
        self.assertEqual(big, 2 ** 100)
        self.assertGreater(big, 2 ** 99)
        self.assertEqual(big * 2 // 2, big)

    def test_type(self):
        self.assertIs(type(5), int)
        self.assertIs(type(2 ** 100), int)

    def test_isinstance(self):
        self.assertTrue(isinstance(5, int))
        self.assertTrue(isinstance(2 ** 100, int))
        self.assertFalse(isinstance(5.0, int))
        self.assertFalse(isinstance("5", int))


unittest.main(globals())
