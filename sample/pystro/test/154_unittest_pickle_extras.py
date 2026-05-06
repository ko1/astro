import unittest
import pickle


class PickleBytesTest(unittest.TestCase):
    def test_bytes(self):
        b = b"abc\x00\xff"
        self.assertEqual(pickle.loads(pickle.dumps(b)), b)

    def test_empty_bytes(self):
        self.assertEqual(pickle.loads(pickle.dumps(b"")), b"")

    def test_long_bytes(self):
        b = bytes(range(256))
        self.assertEqual(pickle.loads(pickle.dumps(b)), b)


class PickleNestedTest(unittest.TestCase):
    def test_nested(self):
        data = {"a": [1, "x", None, True], "b": (1, 2), "c": {"nested": [1, 2]}}
        back = pickle.loads(pickle.dumps(data))
        self.assertEqual(back, data)

    def test_set(self):
        s = {1, 2, 3}
        back = pickle.loads(pickle.dumps(s))
        self.assertEqual(back, s)

    def test_floats(self):
        self.assertEqual(pickle.loads(pickle.dumps(3.14)), 3.14)
        self.assertEqual(pickle.loads(pickle.dumps(-1.5)), -1.5)


unittest.main(globals())
