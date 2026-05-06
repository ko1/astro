import unittest


class BytesHexSepTest(unittest.TestCase):
    def test_no_sep(self):
        self.assertEqual(b"\x01\x02\x03\x04".hex(), "01020304")

    def test_sep(self):
        self.assertEqual(b"\x01\x02\x03\x04".hex(":"), "01:02:03:04")
        self.assertEqual(b"\x01\x02\x03\x04".hex("-"), "01-02-03-04")

    def test_sep_bps(self):
        self.assertEqual(b"\x01\x02\x03\x04".hex(":", 2), "0102:0304")
        # negative bps groups from the left
        self.assertEqual(b"\x01\x02\x03\x04".hex("-", -2), "0102-0304")

    def test_empty(self):
        self.assertEqual(b"".hex(":"), "")

    def test_bytearray(self):
        ba = bytearray(b"\xab\xcd\xef")
        self.assertEqual(ba.hex(":"), "ab:cd:ef")


unittest.main(globals())
