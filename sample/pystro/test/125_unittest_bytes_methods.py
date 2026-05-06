import unittest


class BytesMethodsTest(unittest.TestCase):
    def test_title(self):
        self.assertEqual(b"hello world".title(), b"Hello World")

    def test_capitalize(self):
        self.assertEqual(b"hello".capitalize(), b"Hello")

    def test_swapcase(self):
        self.assertEqual(b"Hello".swapcase(), b"hELLO")

    def test_zfill(self):
        self.assertEqual(b"42".zfill(5), b"00042")
        self.assertEqual(b"-42".zfill(5), b"-0042")

    def test_center(self):
        self.assertEqual(b"hi".center(8, b"*"), b"***hi***")

    def test_ljust_rjust(self):
        self.assertEqual(b"hi".ljust(5, b"."), b"hi...")
        self.assertEqual(b"hi".rjust(5, b"."), b"...hi")

    def test_isalpha(self):
        self.assertTrue(b"abc".isalpha())
        self.assertFalse(b"abc1".isalpha())
        self.assertFalse(b"".isalpha())

    def test_isdigit(self):
        self.assertTrue(b"123".isdigit())
        self.assertFalse(b"123a".isdigit())

    def test_find_with_range(self):
        self.assertEqual(b"hello".find(b"l", 0, 3), 2)
        self.assertEqual(b"hello".find(b"l", 4), -1)

    def test_startswith_tuple(self):
        self.assertTrue(b"hello".startswith((b"he", b"hi")))
        self.assertFalse(b"hello".startswith((b"xy", b"yz")))

    def test_endswith_with_range(self):
        self.assertTrue(b"hello".endswith(b"llo"))
        self.assertFalse(b"hello".endswith(b"llo", 0, 4))


unittest.main(globals())
