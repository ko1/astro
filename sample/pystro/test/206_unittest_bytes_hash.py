import unittest


class BytesHashTest(unittest.TestCase):
    def test_equal_bytes_same_hash(self):
        self.assertEqual(hash(b"abc"), hash(b"abc"))
        self.assertEqual(hash(b""), hash(b""))

    def test_different_bytes_different_hash(self):
        self.assertNotEqual(hash(b"abc"), hash(b"abd"))

    def test_bytes_as_dict_key(self):
        d = {b"abc": 1}
        self.assertEqual(d[b"abc"], 1)
        d[b"abc"] = 2
        self.assertEqual(len(d), 1)
        self.assertEqual(d[b"abc"], 2)

    def test_bytes_set(self):
        s = {b"abc", b"abc", b"def"}
        self.assertEqual(len(s), 2)


class BytesAddResultTest(unittest.TestCase):
    def test_bytes_plus_bytes(self):
        result = b"abc" + b"def"
        self.assertIsInstance(result, bytes)

    def test_bytes_plus_bytearray(self):
        # CPython: result type follows the LEFT operand.
        result = b"abc" + bytearray(b"def")
        self.assertEqual(type(result).__name__, "bytes")

    def test_bytearray_plus_bytes(self):
        result = bytearray(b"abc") + b"def"
        self.assertEqual(type(result).__name__, "bytearray")


unittest.main(globals())
