import unittest


class BytearrayTest(unittest.TestCase):
    def test_setitem(self):
        ba = bytearray(b"hello")
        ba[0] = ord("H")
        self.assertEqual(bytes(ba), b"Hello")

    def test_mul(self):
        self.assertEqual(b"!!" * 3, b"!!!!!!")
        self.assertEqual(3 * b"ab", b"ababab")

    def test_index(self):
        ba = bytearray(b"abc")
        with self.assertRaises(IndexError):
            ba[100] = 0

    def test_value_range(self):
        ba = bytearray(b"x")
        with self.assertRaises(ValueError):
            ba[0] = 999


class FileIterTest(unittest.TestCase):
    def test_for_line(self):
        import tempfile, os
        fd, path = tempfile.mkstemp() if hasattr(tempfile, "mkstemp") else (None, None)
        if not path:
            return  # tempfile not avail; skip
        os.close(fd)
        with open(path, "w") as f:
            f.write("line1\nline2\n")
        lines = []
        with open(path) as f:
            for line in f:
                lines.append(line)
        os.unlink(path)
        self.assertEqual(lines, ["line1\n", "line2\n"])


class StringIOTest(unittest.TestCase):
    def test_iter(self):
        import io
        s = io.StringIO("a\nb\nc\n")
        self.assertEqual(list(s), ["a\n", "b\n", "c\n"])

    def test_readline(self):
        import io
        s = io.StringIO("a\nb")
        self.assertEqual(s.readline(), "a\n")
        self.assertEqual(s.readline(), "b")
        self.assertEqual(s.readline(), "")


unittest.main(globals())
