import unittest


class BytearrayMethodsTest(unittest.TestCase):
    def test_append(self):
        ba = bytearray()
        ba.append(65)
        ba.append(66)
        self.assertEqual(bytes(ba), b"AB")

    def test_extend(self):
        ba = bytearray(b"abc")
        ba.extend(b"def")
        self.assertEqual(bytes(ba), b"abcdef")
        ba.extend([100, 101])
        self.assertEqual(bytes(ba), b"abcdefde")

    def test_insert(self):
        ba = bytearray(b"acdef")
        ba.insert(1, ord("b"))
        self.assertEqual(bytes(ba), b"abcdef")

    def test_pop(self):
        ba = bytearray(b"hello")
        self.assertEqual(ba.pop(), ord("o"))
        self.assertEqual(bytes(ba), b"hell")
        self.assertEqual(ba.pop(0), ord("h"))
        self.assertEqual(bytes(ba), b"ell")

    def test_remove(self):
        ba = bytearray(b"hello")
        ba.remove(ord("l"))
        self.assertEqual(bytes(ba), b"helo")

    def test_remove_missing(self):
        ba = bytearray(b"abc")
        with self.assertRaises(ValueError):
            ba.remove(ord("z"))

    def test_reverse(self):
        ba = bytearray(b"abc")
        ba.reverse()
        self.assertEqual(bytes(ba), b"cba")

    def test_clear(self):
        ba = bytearray(b"hello")
        ba.clear()
        self.assertEqual(bytes(ba), b"")
        self.assertEqual(len(ba), 0)


class BytesCompareTest(unittest.TestCase):
    def test_lt(self):
        self.assertLess(b"abc", b"abd")
        self.assertLess(b"abc", b"abcd")
        self.assertLess(b"a", b"b")

    def test_eq(self):
        self.assertEqual(b"abc", b"abc")
        self.assertNotEqual(b"abc", b"abd")

    def test_sorted(self):
        self.assertEqual(sorted([b"c", b"a", b"b"]), [b"a", b"b", b"c"])


unittest.main(globals())
