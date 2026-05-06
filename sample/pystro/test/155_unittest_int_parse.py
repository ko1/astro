import unittest


class IntParseTest(unittest.TestCase):
    def test_plus(self):
        self.assertEqual(int("+42"), 42)

    def test_minus(self):
        self.assertEqual(int("-42"), -42)

    def test_whitespace(self):
        self.assertEqual(int("  42  "), 42)
        self.assertEqual(int("\t-7\n"), -7)

    def test_with_base(self):
        self.assertEqual(int("0x1f", 0), 31)
        self.assertEqual(int("0o17", 0), 15)
        self.assertEqual(int("0b1010", 0), 10)

    def test_signed_with_base(self):
        self.assertEqual(int("+0x1f", 16), 31)
        self.assertEqual(int("-0b101", 2), -5)
        self.assertEqual(int("-0o17", 0), -15)

    def test_underscore(self):
        self.assertEqual(int("1_000_000"), 1000000)

    def test_empty(self):
        with self.assertRaises(ValueError):
            int("")

    def test_invalid(self):
        with self.assertRaises(ValueError):
            int("not a number")


unittest.main(globals())
