import unittest


class UnicodeEscapeTest(unittest.TestCase):
    def test_u4(self):
        self.assertEqual("é", "é")

    def test_U8(self):
        self.assertEqual("\U0001F600", chr(0x1F600))

    def test_round_trip_u(self):
        self.assertEqual(ord("é"), 233)

    def test_round_trip_U(self):
        self.assertEqual(ord("\U0001F600"), 0x1F600)

    def test_in_fstring(self):
        x = "é"
        self.assertEqual(f"acc{x}nt", "accént")


unittest.main(globals())
