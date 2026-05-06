import unittest


class BytearraySliceAssignTest(unittest.TestCase):
    def test_eq_len(self):
        ba = bytearray(b"hello")
        ba[1:3] = b"XY"
        self.assertEqual(bytes(ba), b"hXYlo")

    def test_grow(self):
        ba = bytearray(b"hello")
        ba[1:3] = b"XYZW"
        self.assertEqual(bytes(ba), b"hXYZWlo")

    def test_shrink(self):
        ba = bytearray(b"hello")
        ba[1:4] = b"X"
        self.assertEqual(bytes(ba), b"hXo")

    def test_clear(self):
        ba = bytearray(b"hello")
        ba[1:4] = b""
        self.assertEqual(bytes(ba), b"ho")

    def test_insert(self):
        ba = bytearray(b"helo")
        ba[2:2] = b"l"
        self.assertEqual(bytes(ba), b"hello")

    def test_with_iterable(self):
        ba = bytearray(b"hello")
        ba[1:3] = [88, 89]  # 'X', 'Y'
        self.assertEqual(bytes(ba), b"hXYlo")

    def test_stepped(self):
        ba = bytearray(b"abcdef")
        ba[::2] = b"XYZ"
        self.assertEqual(bytes(ba), b"XbYdZf")

    def test_stepped_mismatch(self):
        ba = bytearray(b"abcdef")
        with self.assertRaises(ValueError):
            ba[::2] = b"XY"

    def test_byte_oor(self):
        ba = bytearray(b"abc")
        with self.assertRaises(ValueError):
            ba[0:1] = [256]


class DictViewSetOpsTest(unittest.TestCase):
    def test_keys_set_ops(self):
        a = {"a": 1, "b": 2, "c": 3}
        b = {"b": 4, "c": 5, "d": 6}
        self.assertEqual(sorted(a.keys() & b.keys()), ["b", "c"])
        self.assertEqual(sorted(a.keys() | b.keys()), ["a", "b", "c", "d"])
        self.assertEqual(sorted(a.keys() - b.keys()), ["a"])
        self.assertEqual(sorted(a.keys() ^ b.keys()), ["a", "d"])

    def test_dict_or(self):
        a = {1: "a"}
        b = {2: "b", 1: "B"}
        self.assertEqual(a | b, {1: "B", 2: "b"})

    def test_dict_reversed(self):
        d = {"a": 1, "b": 2, "c": 3}
        self.assertEqual(list(reversed(d)), ["c", "b", "a"])

    def test_dict_update_kwargs(self):
        d = {"a": 1}
        d.update(b=2, c=3)
        self.assertEqual(sorted(d.items()), [("a", 1), ("b", 2), ("c", 3)])

    def test_dict_update_iter(self):
        d = {}
        d.update([("a", 1), ("b", 2)])
        self.assertEqual(sorted(d.items()), [("a", 1), ("b", 2)])


unittest.main(globals())
