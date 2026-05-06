import unittest


class FormatSignTest(unittest.TestCase):
    def test_plus(self):
        self.assertEqual(f"{42:+}", "+42")
        self.assertEqual(f"{-42:+}", "-42")

    def test_space(self):
        self.assertEqual(f"{42: }", " 42")
        self.assertEqual(f"{-42: }", "-42")

    def test_zero_pad_with_alt_form(self):
        self.assertEqual(f"{255:#08x}", "0x0000ff")

    def test_neg_hex(self):
        self.assertEqual(f"{-255:x}", "-ff")
        self.assertEqual(f"{-255:#x}", "-0xff")

    def test_neg_oct(self):
        self.assertEqual(f"{-100:o}", "-144")

    def test_neg_bin(self):
        self.assertEqual(f"{-10:b}", "-1010")


unittest.main(globals())
