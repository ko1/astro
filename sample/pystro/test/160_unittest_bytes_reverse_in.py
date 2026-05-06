import unittest


class BytesReversedTest(unittest.TestCase):
    def test_reversed(self):
        self.assertEqual(bytes(reversed(b"abc")), b"cba")

    def test_reversed_empty(self):
        self.assertEqual(bytes(reversed(b"")), b"")

    def test_reversed_iter(self):
        # reversed(bytes) yields ints (per CPython).
        self.assertEqual(list(reversed(b"abc")), [99, 98, 97])


class BytesInTest(unittest.TestCase):
    def test_substring(self):
        self.assertIn(b"a", b"cab")
        self.assertIn(b"ab", b"cab")
        self.assertNotIn(b"x", b"cab")

    def test_int_member(self):
        self.assertIn(65, b"ABC")
        self.assertNotIn(99, b"ABC")
        self.assertNotIn(-1, b"ABC")
        self.assertNotIn(256, b"ABC")


class StrInTest(unittest.TestCase):
    def test_substring(self):
        self.assertIn("a", "cab")
        self.assertIn("ab", "cab")
        self.assertNotIn("x", "cab")


class FormatBuiltinTest(unittest.TestCase):
    def test_int_width(self):
        self.assertEqual(format(42, "5"), "   42")

    def test_str_align(self):
        self.assertEqual(format("a", ">5"), "    a")

    def test_bin_oct_hex(self):
        self.assertEqual(format(10, "b"), "1010")
        self.assertEqual(format(15, "04x"), "000f")

    def test_percent(self):
        self.assertEqual(format(0.25, ".0%"), "25%")


unittest.main(globals())
