import unittest


class FormatNegPadTest(unittest.TestCase):
    def test_zero_pad_neg(self):
        self.assertEqual(f"{-42:05d}", "-0042")

    def test_zero_pad_plus(self):
        self.assertEqual(f"{42:+05d}", "+0042")

    def test_normal_pad(self):
        self.assertEqual(f"{-42:5d}", "  -42")

    def test_zero_pad_pos(self):
        self.assertEqual(f"{42:05d}", "00042")


class StrRfindRangeTest(unittest.TestCase):
    def test_rfind_with_range(self):
        s = "abcdefabc"
        self.assertEqual(s.rfind("abc", 0, 5), 0)
        self.assertEqual(s.rfind("abc"), 6)

    def test_rfind_not_found(self):
        s = "hello"
        self.assertEqual(s.rfind("xyz", 0, 3), -1)


unittest.main(globals())
