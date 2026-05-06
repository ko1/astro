import unittest
import os


class FileIOTest(unittest.TestCase):
    PATH = "/tmp/pystro_test_file_io.txt"

    def setUp(self):
        try: os.remove(self.PATH)
        except: pass

    def tearDown(self):
        try: os.remove(self.PATH)
        except: pass

    def test_write_read(self):
        with open(self.PATH, "w") as f:
            f.write("hello\nworld\n")
        with open(self.PATH) as f:
            self.assertEqual(f.read(), "hello\nworld\n")

    def test_iter_lines(self):
        with open(self.PATH, "w") as f:
            f.write("a\nb\nc\n")
        with open(self.PATH) as f:
            self.assertEqual(list(f), ["a\n", "b\n", "c\n"])

    def test_readline(self):
        with open(self.PATH, "w") as f:
            f.write("first\nsecond\n")
        with open(self.PATH) as f:
            self.assertEqual(f.readline(), "first\n")
            self.assertEqual(f.readline(), "second\n")
            self.assertEqual(f.readline(), "")  # EOF

    def test_append(self):
        with open(self.PATH, "w") as f:
            f.write("a")
        with open(self.PATH, "a") as f:
            f.write("b")
        with open(self.PATH) as f:
            self.assertEqual(f.read(), "ab")

    def test_binary(self):
        with open(self.PATH, "wb") as f:
            f.write(b"\x00\xff\x42")
        with open(self.PATH, "rb") as f:
            self.assertEqual(f.read(), b"\x00\xff\x42")

    def test_seek_tell(self):
        with open(self.PATH, "wb") as f:
            f.write(b"0123456789")
        with open(self.PATH, "rb") as f:
            self.assertEqual(f.tell(), 0)
            f.seek(5)
            self.assertEqual(f.read(), b"56789")
            f.seek(0)
            self.assertEqual(f.read(3), b"012")
            f.seek(0, 2)  # SEEK_END
            self.assertEqual(f.tell(), 10)

    def test_capabilities(self):
        with open(self.PATH, "w") as f:
            self.assertTrue(f.writable())
            self.assertTrue(f.seekable())


unittest.main(globals())
