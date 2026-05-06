import unittest


class ChrOrdUtf8Test(unittest.TestCase):
    def test_ascii(self):
        self.assertEqual(chr(65), "A")
        self.assertEqual(ord("A"), 65)

    def test_latin1(self):
        # é is U+00E9 = 233.  UTF-8 encoded as 2 bytes.
        self.assertEqual(ord("é"), 233)
        self.assertEqual(chr(233), "é")

    def test_emoji(self):
        # 😀 is U+1F600 = 128512.  UTF-8 encoded as 4 bytes.
        c = chr(0x1F600)
        self.assertEqual(ord(c), 0x1F600)

    def test_round_trip(self):
        for cp in [0, 1, 127, 128, 0xFF, 0x100, 0xFFFF, 0x10000, 0x10FFFF]:
            self.assertEqual(ord(chr(cp)), cp)

    def test_out_of_range(self):
        with self.assertRaises(ValueError):
            chr(-1)
        with self.assertRaises(ValueError):
            chr(0x110000)


unittest.main(globals())
